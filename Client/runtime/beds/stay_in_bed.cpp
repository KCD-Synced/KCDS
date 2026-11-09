// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "stay_in_bed.hpp"

#include <cstdint>

#include "../action_hints/resource_action_hints.hpp"
#include "../core/natives.hpp"
#include "../seats/seat_reservations.hpp"
#include "MinHook.h"

namespace kcds::runtime::beds {
namespace {

// C_ScriptBindPlayer::OnBedStop(binding, function handler): the stand-up of a player lying in a
// bed. It returns what the script function handler's EndFunction (slot 11) returns.
using BedStop = std::int64_t(__fastcall*)(void* binding, void* handler);
constexpr std::size_t kHandlerEndFunction = 11;

// The game's own "Get up", in the player's language.
constexpr char kGetUpText[] = "@ui_hud_sitting_action_exit";
constexpr std::int64_t kGetUpHint = -71;

BedStop original_bed_stop = nullptr;
Logger log_line = nullptr;
// The binding the bed script asked to stand up with, while that is held.
void* held_binding = nullptr;
bool hint_shown = false;
// Set while the held stand-up runs, so the hook lets it through.
bool standing_up = false;

std::int64_t __fastcall end_function(void*) {
  return 0;
}

// A script function handler for the held stand-up: OnBedStop only ends the function through it.
struct QuietHandler {
  void* const* table;
};

void* const kQuietTable[16] = {
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
    reinterpret_cast<void*>(&end_function), reinterpret_cast<void*>(&end_function),
};

bool lying_in_bed() {
  return !seats::local_seats().empty();
}

void drop_hint() {
  if (hint_shown) {
    action_hints::clear_engine_hint(kGetUpHint);
    hint_shown = false;
  }
}

// The held stand-up, as the bed script would have run it.
void get_up() {
  void* binding = held_binding;
  held_binding = nullptr;
  drop_hint();
  if (!binding || !original_bed_stop) {
    return;
  }
  QuietHandler handler{kQuietTable};
  standing_up = true;
  original_bed_stop(binding, &handler);
  standing_up = false;
  if (log_line) {
    log_line("BED got up");
  }
}

std::int64_t __fastcall hooked_bed_stop(void* binding, void* handler) {
  if (standing_up || !game_client::multiplayer_world_ready.load() || !lying_in_bed()) {
    return original_bed_stop(binding, handler);
  }
  held_binding = binding;
  if (log_line) {
    log_line("BED stays lying: no sleep in a multiplayer world; Get up stands up");
  }
  using EndFunction = std::int64_t(__fastcall*)(void* handler);
  return game_client::slot<EndFunction>(handler, kHandlerEndFunction)(handler);
}

}  // namespace

bool install(Logger logger) {
  log_line = logger;
  void* target = game_client::resolve_unique(
      "PlayerOnBedStop",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B FA E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 75 0B 48 8B 07 "
      "48 8B CF FF 50 58 EB 69 48 83 B8 C0 0D 00 00 00 74 EB E8 ?? ?? ?? ?? 48 83 C0 0C 48 8D 15 "
      "?? ?? ?? ?? 41 B8 06 00 00 00");
  const bool hooked = target &&
                      MH_CreateHook(target, reinterpret_cast<void*>(&hooked_bed_stop),
                                    reinterpret_cast<void**>(&original_bed_stop)) == MH_OK &&
                      MH_EnableHook(target) == MH_OK;
  if (log_line) {
    log_line("BED stay lying %s", hooked ? "hooked" : "unavailable");
  }
  return hooked;
}

void frame(bool in_world) {
  if (!held_binding) {
    return;
  }
  if (!in_world || !lying_in_bed()) {
    // Out of the bed another way: nothing to stand up from any more.
    held_binding = nullptr;
    drop_hint();
    return;
  }
  if (!hint_shown) {
    action_hints::EngineHint hint;
    hint.id = kGetUpHint;
    hint.key = "primary_action";
    hint.text = kGetUpText;
    hint.localized = true;
    hint.on_fire = [] { get_up(); };
    action_hints::set_engine_hint(hint);
    hint_shown = true;
  }
}

}  // namespace kcds::runtime::beds
