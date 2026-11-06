#include "game_dialogs.hpp"

#include <cstddef>
#include <cstdint>

#include "../hud/monologue.hpp"
#include "dialogue_list.hpp"
#include "../knockout/knockout.hpp"
#include "MinHook.h"

namespace kcds::runtime::dialogue::game_dialogs {
namespace {

// The request's kind (the start compares it with 1).
constexpr std::size_t kRequestKind = 172;
// The request's participants: a vector of their ids (the start's check).
constexpr std::size_t kParticipantsBegin = 88;
constexpr std::size_t kParticipantsEnd = 96;

using Start = void*(__fastcall*)(void* manager, void* request);

Logger log_line = nullptr;
Start original_start = nullptr;

void* __fastcall hooked_start(void* manager, void* request) {
  const int kind =
      request ? *reinterpret_cast<const int*>(static_cast<std::uint8_t*>(request) + kRequestKind)
              : -1;
  // Also while kept down: the game ends its own unconsciousness before the revive and starts the
  // defeat dialog with the NPC replica that won in that moment, and the dialog's update then
  // read a participant the replica lacks (at +0x38; a crash).
  if (kcds::runtime::knockout::knocked_out() || kcds::runtime::knockout::kept_down_now()) {
    if (log_line) {
      log_line("DIALOG game dialog kind=%d refused: the local player lies knocked out", kind);
    }
    return nullptr;
  }
  // A server conversation (Dialogue) on the screen: a nearby NPC's line would start the game's
  // own conversation and end ours (the team silenced the NPCs around); the
  // game's waits until ours is over.
  if (kcds::runtime::dialogue::showing()) {
    if (log_line) {
      log_line("DIALOG game dialog kind=%d refused: a server conversation is shown", kind);
    }
    return nullptr;
  }
  // The player's comments on their own state, in Henry's voice: muted with the monologue
  // (Hud.setInnerVoice), like their other remarks.
  if (request) {
    auto* bytes = static_cast<std::uint8_t*>(request);
    const auto* first = *reinterpret_cast<const std::uint64_t* const*>(bytes + kParticipantsBegin);
    const auto* last = *reinterpret_cast<const std::uint64_t* const*>(bytes + kParticipantsEnd);
    if (kcds::runtime::hud::monologue::player_alone(first, last)) {
      if (log_line) {
        log_line("DIALOG game dialog kind=%d refused: the player's own monologue", kind);
      }
      return nullptr;
    }
  }
  void* instance = original_start(manager, request);
  if (log_line) {
    log_line("DIALOG game dialog kind=%d started=%u", kind, unsigned(instance != nullptr));
  }
  return instance;
}

}  // namespace

bool install(void* start, Logger logger) {
  log_line = logger;
  if (!start) {
    return false;
  }
  return MH_CreateHook(start, reinterpret_cast<void*>(hooked_start),
                       reinterpret_cast<void**>(&original_start)) == MH_OK &&
         MH_EnableHook(start) == MH_OK;
}

}  // namespace kcds::runtime::dialogue::game_dialogs
