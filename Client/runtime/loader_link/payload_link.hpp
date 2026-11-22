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

// The client's long-lived threads: stopping() turns true when the client is about to be unloaded,
// and every loop ends then; the unload waits for every thread that holds a ThreadScope (a thread
// still running keeps the client's code mapped and running after the swap). Any thread.
bool stopping();
class ThreadScope {
 public:
  explicit ThreadScope(const char* name);
  ~ThreadScope();
  ThreadScope(const ThreadScope&) = delete;
  ThreadScope& operator=(const ThreadScope&) = delete;

 private:
  const char* name_;
};

// A step that takes back something of this client before it is unloaded; false when it could not
// (the swap stops and the client keeps running). Any thread.
void on_unload(const char* name, std::function<bool()> step);

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

// Points a pointer of the game (a vtable slot) at detour; *original gets what it held. Through
// the loader (version 6) when there is one, so the slot never names this client once it is
// unloaded; else written directly. false when it could not be written.
bool hook_pointer(void** where, void* detour, void** original);

// Whether the loader can swap this client for another (a loader of version 5).
bool can_swap();

// Whether a swap loaded this client (the game was already at its main menu when it started).
bool loaded_by_swap();

// Every frame (engine thread): the test hook's swap request.
void frame();

}  // namespace kcds::runtime::loader_link
