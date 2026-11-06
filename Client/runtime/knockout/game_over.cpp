#include "game_over.hpp"

#include "MinHook.h"

namespace kcds::runtime::knockout::game_over {
namespace {

using Start = void(__fastcall*)(void* manager, unsigned reason);

Start original_start = nullptr;
bool (*server_world)() = nullptr;
Logger log_line = nullptr;

void __fastcall hooked_start(void* manager, unsigned reason) {
  if (server_world && server_world()) {
    if (log_line) {
      log_line("KNOCKOUT game over refused in a server world (reason %u)", reason);
    }
    return;
  }
  original_start(manager, reason);
}

}  // namespace

bool install(void* start, bool (*in_server_world)(), Logger logger) {
  server_world = in_server_world;
  log_line = logger;
  return start &&
         MH_CreateHook(start, reinterpret_cast<void*>(hooked_start),
                       reinterpret_cast<void**>(&original_start)) == MH_OK &&
         MH_EnableHook(start) == MH_OK;
}

}  // namespace kcds::runtime::knockout::game_over
