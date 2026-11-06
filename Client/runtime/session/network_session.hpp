// The connection to a server: the KSTP client opened with its resource runtime and local runtime
// handlers, the network worker that ticks it, and the web views' and keys' reports pumped to it.
// open_client and pump_web_views are declared in core/client_internal.hpp.
#pragma once

#include <Windows.h>

namespace kcds::game_client {

// Without KCDS_SERVER_ADDRESS the player picks the server in the in-game browser first.
extern bool browser_mode;
DWORD WINAPI network_worker(void*);
// Called by initialize_adapter(): Winsock started, and without the browser the
// client connected to the launcher's (or the local development) server. Returns 0, or 1 when
// the adapter must not start (its exit code).
DWORD open_network();
// Called by initialize_adapter() last: the network worker started. Returns 0, or 1 when it could
// not start (the engine loop hook is disabled again).
DWORD start_network_worker();

}  // namespace kcds::game_client
