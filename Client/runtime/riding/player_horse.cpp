#include "player_horse.hpp"

#include <cstddef>

namespace kcds::runtime::riding {
namespace {

using SetPlayerHorse = char(__fastcall*)(void* component, std::uint32_t horse_entity);
using CompanionOf = void*(__fastcall*)(void* actor, char kind);

// C_Player + 3432: the local player's horse component (the remote players' C_Player have none;
// the player's mount wrapper and the setter read it). Its + 32 is the player again.
constexpr std::size_t kHorseComponent = 3432;
constexpr std::size_t kComponentPlayer = 32;
// An actor's part the game links companions by (the getter reads the player's, the setter the
// horse's).
constexpr std::size_t kCompanionPart = 1640;
// Companion kind 0 is the horse (0 horse, 1 dog).
constexpr char kHorseCompanion = 0;

SetPlayerHorse set_player_horse = nullptr;
CompanionOf companion_of = nullptr;
Logger log_line = nullptr;

void* field(void* object, std::size_t offset) {
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(object) + offset);
}

// The local player's horse component, when it belongs to that player.
void* horse_component(void* local_actor) {
  if (!local_actor || !set_player_horse) {
    return nullptr;
  }
  void* component = field(local_actor, kHorseComponent);
  if (!component) {
    return nullptr;
  }
  if (field(component, kComponentPlayer) != local_actor) {
    static bool reported = false;
    if (!reported && log_line) {
      reported = true;
      log_line("HORSE player horse: the horse component names another actor; not used");
    }
    return nullptr;
  }
  return component;
}

// The horse the game holds as the local player's, or null.
void* held_horse(void* local_actor) {
  if (!companion_of || !local_actor || !field(local_actor, kCompanionPart)) {
    return nullptr;
  }
  return companion_of(local_actor, kHorseCompanion);
}

}  // namespace

bool init_player_horses(const PlayerHorseNatives& natives, Logger logger) {
  log_line = logger;
  set_player_horse = reinterpret_cast<SetPlayerHorse>(natives.set_player_horse);
  companion_of = reinterpret_cast<CompanionOf>(natives.companion_of);
  if (!set_player_horse || !companion_of) {
    set_player_horse = nullptr;
    companion_of = nullptr;
    return false;
  }
  return true;
}

bool horse_ready(void* horse_actor) {
  return horse_actor && field(horse_actor, kCompanionPart);
}

bool make_player_horse(void* local_actor, void* horse_actor, std::uint32_t horse_entity) {
  void* component = horse_component(local_actor);
  if (!component || !horse_entity || !horse_ready(horse_actor)) {
    return false;
  }
  void* held = held_horse(local_actor);
  if (held == horse_actor) {
    return true;
  }
  // Letting another horse go first: given a new horse while holding one, the game would move the
  // old horse's equipped items onto the new one (->) in this game alone.
  if (held) {
    set_player_horse(component, 0);
  }
  return set_player_horse(component, horse_entity) != 0;
}

void clear_player_horse(void* local_actor) {
  void* component = horse_component(local_actor);
  if (component && held_horse(local_actor)) {
    set_player_horse(component, 0);
  }
}

bool release_player_horse(void* local_actor, void* horse_actor) {
  void* component = horse_component(local_actor);
  if (!component || !horse_actor || held_horse(local_actor) != horse_actor) {
    return false;
  }
  set_player_horse(component, 0);
  return true;
}

}  // namespace kcds::runtime::riding
