/*
 * Copyright 2003-2017 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "QueueCommands.hxx"
#include "Request.hxx"
#include "CommandError.hxx"
#include "db/DatabaseQueue.hxx"
#include "db/Selection.hxx"
#include "SongFilter.hxx"
#include "SongLoader.hxx"
#include "DetachedSong.hxx"
#include "LocateUri.hxx"
#include "queue/Playlist.hxx"
#include "PlaylistPrint.hxx"
#include "client/Client.hxx"
#include "client/Response.hxx"
#include "Partition.hxx"
#include "Instance.hxx"
#include "BulkEdit.hxx"
#include "util/ConstBuffer.hxx"
#include "util/StringAPI.hxx"
#include "util/NumberParser.hxx"
#include "util/StringCompare.hxx"
#include "tag/Builder.hxx"
#include "StateFile.hxx"
#include "config/ConfigOption.hxx"
#include "config/ConfigGlobal.hxx"

#include <memory>
#include <limits>

static void
AddUri(Client &client, const char *uri, const Tag &tag)
{
	auto &partition = client.GetPartition();
	partition.playlist.AppendSong(partition.pc,
				       tag.IsEmpty()
				           ? SongLoader(client).LoadSong(uri)
				           : SongLoader(client).LoadSong(uri, tag));
}

static CommandResult
AddDatabaseSelection(Client &client, const RangeArg &range, const char *uri, gcc_unused Response &r)
{
#ifdef ENABLE_DATABASE
	auto &partition = client.GetPartition();
	const ScopeBulkEdit bulk_edit(partition);

	DatabaseSelection selection(uri, true);
	selection.window_start = range.start;
	selection.window_end = range.end;
	AddFromDatabase(partition, selection);
	return CommandResult::OK;
#else
	(void)client;
	(void)uri;

	r.Error(ACK_ERROR_NO_EXIST, "No database");
	return CommandResult::ERROR;
#endif
}

CommandResult
handle_add(Client &client, Request args, Response &r)
{
	const char *uri = args.front();
	if (StringIsEqual(uri, "/"))
		/* this URI is malformed, but some clients are buggy
		   and use "add /" to add the whole database, which
		   was never intended to work, but once did; in order
		   to retain backwards compatibility, work around this
		   here */
		uri = "";

	Tag tag;
	if (args.size >= 2 &&
		StringStartsWith(args.back(), "{") &&
		StringEndsWith(args.back(), "}")) {
		try {
			jaijson::Document d;
			if (!d.Parse(args.back()).HasParseError()) {
				deserialize(d, tag);
				args.pop_back();
			}
		} catch (...) {
			throw FormatProtocolError(ACK_ERROR_ARG, "parse json %s fail", args.back());
		}
	}
	RangeArg range = args.ParseOptional(1, RangeArg::All());
	const auto located_uri = StringStartsWith(uri, "upnp://")
		? LocatedUri(LocatedUri::Type::RELATIVE, uri)
		: LocateUri(uri, &client
#ifdef ENABLE_DATABASE
					   , nullptr
#endif
					   );
	switch (located_uri.type) {
	case LocatedUri::Type::ABSOLUTE:
	case LocatedUri::Type::PATH:
		AddUri(client, uri, tag);
		return CommandResult::OK;

	case LocatedUri::Type::RELATIVE:
		return AddDatabaseSelection(client, range, located_uri.canonical_uri,
					    r);
	}

	gcc_unreachable();
}

CommandResult
handle_addid(Client &client, Request args, Response &r)
{
	const char *uri = args.front();
	if (StringIsEqual(uri, "/"))
		/* this URI is malformed, but some clients are buggy
		   and use "add /" to add the whole database, which
		   was never intended to work, but once did; in order
		   to retain backwards compatibility, work around this
		   here */
		uri = "";

	Tag tag;
	if (args.size >= 2 &&
		StringStartsWith(args.back(), "{") &&
		StringEndsWith(args.back(), "}")) {
		try {
			jaijson::Document d;
			if (!d.Parse(args.back()).HasParseError()) {
				deserialize(d, tag);
				args.pop_back();
			}
		} catch (...) {
			throw FormatProtocolError(ACK_ERROR_ARG, "parse json %s fail", args.back());
		}
	}
	auto &partition = client.GetPartition();
	bool is_upnp = StringStartsWith(uri, "upnp://");
	const Database *db = is_upnp ? client.GetUpnpDatabase() : client.GetDatabase();
	if (is_upnp) {
		uri = uri + 7;
	}
	const SongLoader loader(client, db, client.GetStorage());
	unsigned added_id = tag.IsEmpty() ? partition.AppendURI(loader, uri)
			: partition.AppendURI(loader, uri, tag);
	partition.instance.LookupRemoteTag(uri);

	if (args.size == 2) {
		unsigned to = args.ParseUnsigned(1);

		try {
			partition.MoveId(added_id, to);
		} catch (...) {
			/* rollback */
			partition.DeleteId(added_id);
			throw;
		}
	}

	r.Format("Id: %u\n", added_id);
	return CommandResult::OK;
}

/**
 * Parse a string in the form "START:END", both being (optional)
 * fractional non-negative time offsets in seconds.  Returns both in
 * integer milliseconds.  Omitted values are zero.
 */
static bool
parse_time_range(const char *p, SongTime &start_r, SongTime &end_r)
{
	char *endptr;

	const float start = ParseFloat(p, &endptr);
	if (*endptr != ':' || start < 0)
		return false;

	start_r = endptr > p
		? SongTime::FromS(start)
		: SongTime::zero();

	p = endptr + 1;

	const float end = ParseFloat(p, &endptr);
	if (*endptr != 0 || end < 0)
		return false;

	end_r = endptr > p
		? SongTime::FromS(end)
		: SongTime::zero();

	return end_r.IsZero() || end_r > start_r;
}

CommandResult
handle_rangeid(Client &client, Request args, Response &r)
{
	unsigned id = args.ParseUnsigned(0);

	SongTime start, end;
	if (!parse_time_range(args[1], start, end)) {
		r.Error(ACK_ERROR_ARG, "Bad range");
		return CommandResult::ERROR;
	}

	client.GetPlaylist().SetSongIdRange(client.GetPlayerControl(),
					    id, start, end);
	return CommandResult::OK;
}

CommandResult
handle_delete(Client &client, Request args, gcc_unused Response &r)
{
	RangeArg range = args.ParseRange(0);
	client.GetPartition().DeleteRange(range.start, range.end);
	return CommandResult::OK;
}

CommandResult
handle_deleteid(Client &client, Request args, gcc_unused Response &r)
{
	unsigned id = args.ParseUnsigned(0);
	client.GetPartition().DeleteId(id);
	return CommandResult::OK;
}

CommandResult
handle_playlist(Client &client, gcc_unused Request args, Response &r)
{
	playlist_print_uris(r, client.GetPartition(), client.GetPlaylist());
	return CommandResult::OK;
}

CommandResult
handle_shuffle(gcc_unused Client &client, Request args, gcc_unused Response &r)
{
	RangeArg range = args.ParseOptional(0, RangeArg::All());
	client.GetPartition().Shuffle(range.start, range.end);
	return CommandResult::OK;
}

CommandResult
handle_clear(Client &client, gcc_unused Request args, gcc_unused Response &r)
{
	client.GetPartition().ClearQueue();
	return CommandResult::OK;
}

CommandResult
handle_plchanges(Client &client, Request args, Response &r)
{
	uint32_t version = ParseCommandArgU32(args.front());
	RangeArg range = args.ParseOptional(1, RangeArg::All());
	playlist_print_changes_info(r, client.GetPartition(), client.GetPlaylist(), version,
				    range.start, range.end);
	return CommandResult::OK;
}

CommandResult
handle_plchangesposid(Client &client, Request args, Response &r)
{
	uint32_t version = ParseCommandArgU32(args.front());
	RangeArg range = args.ParseOptional(1, RangeArg::All());
	playlist_print_changes_position(r, client.GetPlaylist(), version,
					range.start, range.end);
	return CommandResult::OK;
}

CommandResult
handle_playlistinfo(Client &client, Request args, Response &r)
{
	RangeArg range = args.ParseOptional(0, RangeArg::All());

	playlist_print_info(r, client.GetPartition(), client.GetPlaylist(),
			    range.start, range.end);
	return CommandResult::OK;
}

CommandResult
handle_playlistid(Client &client, Request args, Response &r)
{
	if (!args.empty()) {
		unsigned id = args.ParseUnsigned(0);
		playlist_print_id(r, client.GetPartition(), client.GetPlaylist(), id);
	} else {
		playlist_print_info(r, client.GetPartition(), client.GetPlaylist(),
				    0, std::numeric_limits<unsigned>::max());
	}

	return CommandResult::OK;
}

static CommandResult
handle_playlist_match(Client &client, Request args, Response &r,
		      bool fold_case)
{
	SongFilter filter;
	if (!filter.Parse(args, fold_case)) {
		r.Error(ACK_ERROR_ARG, "incorrect arguments");
		return CommandResult::ERROR;
	}

	playlist_print_find(r, client.GetPartition(), client.GetPlaylist(), filter);
	return CommandResult::OK;
}

CommandResult
handle_playlistfind(Client &client, Request args, Response &r)
{
	return handle_playlist_match(client, args, r, false);
}

CommandResult
handle_playlistsearch(Client &client, Request args, Response &r)
{
	return handle_playlist_match(client, args, r, true);
}

CommandResult
handle_prio(Client &client, Request args, gcc_unused Response &r)
{
	unsigned priority = args.ParseUnsigned(0, 0xff);
	args.shift();

	auto &partition = client.GetPartition();

	for (const char *i : args) {
		RangeArg range = ParseCommandArgRange(i);
		partition.SetPriorityRange(range.start, range.end, priority);
	}

	return CommandResult::OK;
}

CommandResult
handle_prioid(Client &client, Request args, gcc_unused Response &r)
{
	unsigned priority = args.ParseUnsigned(0, 0xff);
	args.shift();

	auto &partition = client.GetPartition();

	for (const char *i : args) {
		unsigned song_id = ParseCommandArgUnsigned(i);
		partition.SetPriorityId(song_id, priority);
	}

	return CommandResult::OK;
}

CommandResult
handle_move(Client &client, Request args, gcc_unused Response &r)
{
	RangeArg range = args.ParseRange(0);
	int to = args.ParseInt(1);
	client.GetPartition().MoveRange(range.start, range.end, to);
	return CommandResult::OK;
}

CommandResult
handle_moveid(Client &client, Request args, gcc_unused Response &r)
{
	unsigned id = args.ParseUnsigned(0);
	int to = args.ParseInt(1);
	client.GetPartition().MoveId(id, to);
	return CommandResult::OK;
}

CommandResult
handle_swap(Client &client, Request args, gcc_unused Response &r)
{
	unsigned song1 = args.ParseUnsigned(0);
	unsigned song2 = args.ParseUnsigned(1);
	client.GetPartition().SwapPositions(song1, song2);
	return CommandResult::OK;
}

CommandResult
handle_swapid(Client &client, Request args, gcc_unused Response &r)
{
	unsigned id1 = args.ParseUnsigned(0);
	unsigned id2 = args.ParseUnsigned(1);
	client.GetPartition().SwapIds(id1, id2);
	return CommandResult::OK;
}

CommandResult
handle_savequeue(Client &client, gcc_unused Request args, gcc_unused Response &r)
{
	auto &ins = client.GetInstance();
	ins.state_file->Write();
	return CommandResult::OK;
}

CommandResult
handle_loadqueue(Client &client, Request args, gcc_unused Response &r)
{
	auto &ins = client.GetInstance();
	delete ins.state_file;

	const auto interval =
		config_get_unsigned(ConfigOption::STATE_FILE_INTERVAL,
				    StateFile::DEFAULT_INTERVAL);
	auto path_fs = AllocatedPath::FromUTF8Throw(args.front());
	ins.state_file = new StateFile(std::move(path_fs), interval,
					     ins.partitions.front(),
					     ins.event_loop);
	ins.state_file->Read();

	return CommandResult::OK;
}
