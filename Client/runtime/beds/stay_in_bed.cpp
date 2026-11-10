// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "stay_in_bed.hpp"

#include "../action_hints/resource_action_hints.hpp"
#include "../core/natives.hpp"
#include "../seats/seat_reservations.hpp"
#include "MinHook.h"

namespace kcds::runtime::beds {
namespace {

// The player's sleep in a bed starting (its behavior node, once the player lies down): it asks
// for the sleep's skip-time screen and, when the screen does not start, stands the player up
// (player:sittingAction "action(3)") and ends the node. The node's state is its argument.
using SleepStart = void*(__fastcall*)(void* node);

// The game's own "Get up", in the player's language.
constexpr char kGetUpText[] = "@ui_hud_sitting_action_exit";
constexpr std::int64_t kGetUpHint = -71;

SleepStart original_sleep_start = nullptr;
Logger log_line = nullptr;
// The sleep node held while the local player lies in the bed.
void* held_node = nullptr;
bool hint_shown = false;
// Set while the held sleep start runs, so the hook lets it through.
bool standing_up = false;

bool lying_in_bed() {
  return !seats::local_seats().empty();
}

void drop_hint() {
  if (hint_shown) {
    action_hints::clear_engine_hint(kGetUpHint);
    hint_shown = false;
  }
}

// The bed's action map ("sitting_bed", whose sitting_sleep is on the use key) while the sleep is
// held: lying in a bed the game has no action on E, so nothing would take it. On, its E reaches
// the player's input, where the Get up hint takes it; off again once the player is up, as the map
// is exclusive. The action map manager's EnableActionMap(name, enable): services + 8 the input
// module, its slot 224 / 8 the manager, the manager's slot 232 / 8.
bool bed_keys_on = false;

// The map's own hints, which do nothing for a player held lying: Sleep on E and Get up on the
// movement keys. Hidden while the map is on; the Get up hint on E stands for them.
constexpr const char* kBedMapActions[] = {"sitting_sleep", "sitting_exit_up", "sitting_exit_down",
                                          "sitting_exit_left", "sitting_exit_right"};

void enable_bed_keys(bool enable) {
  if (!enable && !bed_keys_on) {
    return;
  }
  if (enable != bed_keys_on) {
    for (const char* action : kBedMapActions) {
      action_hints::hide_game_hint(action, enable);
    }
  }
  bed_keys_on = enable;
  constexpr std::size_t kServicesInput = 8;
  constexpr std::size_t kActionMaps = 224;
  constexpr std::size_t kEnableActionMap = 232;
  using GetMaps = void*(__fastcall*)(void*);
  using EnableActionMap = void(__fastcall*)(void*, const char*, bool);
  void* services = game_client::get_services();
  void* input =
      services ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + kServicesInput)
               : nullptr;
  void* maps = input ? game_client::slot<GetMaps>(input, kActionMaps / 8)(input) : nullptr;
  if (maps) {
    game_client::slot<EnableActionMap>(maps, kEnableActionMap / 8)(maps, "sitting_bed", enable);
  }
}

// Get up: the held sleep start runs now. The skip-time screen does not start in a multiplayer
// world, so the game stands the player up its own way and ends the node.
void get_up() {
  void* node = held_node;
  held_node = nullptr;
  drop_hint();
  enable_bed_keys(false);
  if (!node || !original_sleep_start || !lying_in_bed()) {
    return;
  }
  standing_up = true;
  original_sleep_start(node);
  standing_up = false;
  if (log_line) {
    log_line("BED got up");
  }
}

void* __fastcall hooked_sleep_start(void* node) {
  if (standing_up || !game_client::multiplayer_world_ready.load() || !lying_in_bed()) {
    return original_sleep_start(node);
  }
  if (held_node != node && log_line) {
    log_line("BED stays lying: no sleep in a multiplayer world; Get up stands up");
  }
  held_node = node;
  return nullptr;
}

}  // namespace

bool install(Logger logger) {
  log_line = logger;
  void* target = game_client::resolve_unique(
      "PlayerSleepStart",
      "48 8B C4 48 89 58 10 48 89 70 18 55 57 41 56 48 8B EC 48 83 EC 60 83 79 3C 00 48 8B F1 0F "
      "29 70 D8 0F 29 78 C8 0F 87 ?? ?? ?? ?? 80 79 08 00 0F");
  const bool hooked = target &&
                      MH_CreateHook(target, reinterpret_cast<void*>(&hooked_sleep_start),
                                    reinterpret_cast<void**>(&original_sleep_start)) == MH_OK &&
                      MH_EnableHook(target) == MH_OK;
  if (log_line) {
    log_line("BED stay lying %s", hooked ? "hooked" : "unavailable");
  }
  return hooked;
}

void frame(bool in_world) {
  if (!held_node) {
    return;
  }
  if (!in_world || !lying_in_bed()) {
    // Out of the bed another way (knocked out, moved by the server, left the world): the node
    // ended with it, so there is nothing to stand up from any more.
    held_node = nullptr;
    drop_hint();
    enable_bed_keys(false);
    return;
  }
  enable_bed_keys(true);
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
