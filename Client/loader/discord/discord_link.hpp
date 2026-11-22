// The process's one connection to the player's Discord app, held by the loader so it stays across
// client swaps: a client only says what to show and asks who the player is (kcds_abi.h, version
// 7). No thread of a client ever waits on Discord's pipe, so none can be left inside an unloaded
// client.
//
// A worker thread connects (again every 15 seconds while it cannot), publishes the activity when
// it changed (at most every 4 seconds) and sets it again every 30 seconds, answers Discord's pings
// and carries an AUTHORIZE round trip.
#pragma once

#include <string>

namespace kcds::loader::discord {

using Log = void (*)(const char* line);

// Starts the worker for this Discord application; later calls change nothing.
void start(const std::string& app_id, Log log);

// The activity object to show (JSON), "" to clear it.
void set_activity(const std::string& activity_json);

// Whether Discord is connected; user_id gets the signed-in user's id ("" when not connected).
bool status(std::string& user_id);

// ask: sends Discord's AUTHORIZE (false when Discord is not connected; one already asked for is
// shared). !ask: drops the one asked for (a late answer is ignored).
bool authorize(bool ask);

// 1 with Discord's code, -1 when the player declined or the connection went, 0 while waiting (or
// when none was asked for).
int authorization(std::string& code);

}  // namespace kcds::loader::discord
