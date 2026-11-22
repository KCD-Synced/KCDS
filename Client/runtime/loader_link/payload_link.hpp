// The client as a payload of the KCDS loader (Client/loader, kcds/loader/kcds_abi.h): it registers
// once it runs, so the loader can unload it and load the client of another KCDS release in the main
// menu (the server a player joins runs that release).
//
// Before the loader unloads it, every feature that left something behind takes it back: the steps
// added with on_unload run in the reverse order of adding (the last started stops first).
//
// Test hook (KCDS_TEST_HOOKS): "<path to a client DLL>|<join>" in %TEMP%\kcds-swap-<pid>.txt asks
// for that swap, as the server browser does.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace kcds::runtime::loader_link {

using Logger = void (*)(const char* format, ...);

// Registers this client with the loader, once it runs; false without a loader.
bool register_payload(void* module, Logger logger);

// A step that takes back something of this client before it is unloaded. Any thread.
void on_unload(const char* name, std::function<void()> step);

// Asks the loader to unload this client and load the one at path, which then joins join (host:port
// or a list id, or empty). false when the loader refused.
bool request_swap(const std::wstring& path, const std::string& join);

// The join the swap that loaded this client handed over, once; empty when none.
std::string take_join();

// First thing at the start (DllMain): a client loaded after another (the loader's swap) scans the
// game's own bytes, not the earlier client's hooks: a copy of the game module with the bytes the
// loader patched written back becomes the signature scans' view (kcd2s/bridge/sigscan.hpp).
// Nothing for the first client (the game module is not loaded yet, nothing is patched).
void prepare_scan_view(Logger logger);

// Every frame (engine thread): the test hook's swap request.
void frame();

}  // namespace kcds::runtime::loader_link
