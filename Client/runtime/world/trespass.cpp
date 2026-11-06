#include "trespass.hpp"

#include "MinHook.h"

#include <atomic>

namespace kcds::runtime::world::trespass {
namespace {

using ReactionsDisabled = bool(__fastcall*)();

ReactionsDisabled original_reactions_disabled = nullptr;
Logger write_log = nullptr;
std::atomic<bool> allowed_now{false};
std::atomic<bool> logged_allowed{false};
std::atomic<bool> logged_forbidden{false};

// The game's AI threads ask this: an NPC is deciding how to react to someone in a private area.
bool __fastcall hooked_reactions_disabled() {
  const bool allowed = allowed_now.load();
  std::atomic<bool>& logged = allowed ? logged_allowed : logged_forbidden;
  if (!logged.exchange(true) && write_log) {
    write_log("TRESPASS an NPC asks about a trespasser, player allowed=%d", allowed ? 1 : 0);
  }
  return allowed || original_reactions_disabled();
}

}  // namespace

bool install(void* reactions_disabled, Logger log) {
  write_log = log;
  return reactions_disabled &&
         MH_CreateHook(reactions_disabled, reinterpret_cast<void*>(&hooked_reactions_disabled),
                       reinterpret_cast<void**>(&original_reactions_disabled)) == MH_OK &&
         MH_EnableHook(reactions_disabled) == MH_OK;
}

void set_allowed(bool allowed) {
  allowed_now.store(allowed);
}

}  // namespace kcds::runtime::world::trespass
