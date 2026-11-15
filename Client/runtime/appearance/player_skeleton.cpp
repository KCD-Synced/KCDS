#include "player_skeleton.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../body_state/body_readings.hpp"
#include "../core/game_layouts.hpp"
#include "../npcs/npc_body.hpp"
#include "female_skeleton.hpp"
#include "outfit.hpp"

namespace kcds::runtime::appearance::player_skeleton {
namespace {

constexpr char kMaleModel[] = "Objects/Characters/humans/male/skeleton/male.cdf";
// An actor's entity (core/frame.cpp); the character slot the body is in.
constexpr std::size_t kActorEntity = kcds::layouts::Actor::kEntity;
constexpr int kBodySlot = 0;
// CActionController slot 3: Flush.
constexpr std::size_t kControllerFlush = 3;
// IActor slot 154: Revive(reason), as the actor binding Revive(true) calls it.
constexpr std::size_t kActorRevive = 154;
constexpr int kReviveReason = 4;
// The frames after the swap the actor is revived on, once its new character is in.
constexpr unsigned kReviveFrames = 3;

using LoadCharacter = int(__fastcall*)(void* entity, int slot, const char* file, int flags);
using Flush = void(__fastcall*)(void* controller);
using Revive = void(__fastcall*)(void* actor, int reason);

LoadCharacter load_character = nullptr;
Logger log_line = nullptr;
// actor -> whether its body is on the female skeleton (absent: the game's own, male).
std::unordered_map<void*, bool> on_female;
// The actor keep() swapped, until it is revived.
void* reviving = nullptr;
unsigned revive_frames = 0;
// The figure keep_figure() swapped and the clothing it took off, to go on the new character
// once it is in; a piece the game refuses is tried again until kRedressFrames.
void* redressing = nullptr;
std::vector<void*> figure_clothing;
unsigned redress_frames = 0;
constexpr unsigned kRedressFrames = 30;

}  // namespace

bool install(void* found, Logger logger) {
  load_character = reinterpret_cast<LoadCharacter>(found);
  log_line = logger;
  return load_character != nullptr;
}

bool keep(void* actor, bool female) {
  if (!actor || !load_character) {
    return false;
  }
  const auto known = on_female.find(actor);
  const bool current = known != on_female.end() && known->second;
  if (current == female) {
    return false;
  }
  void* entity = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActorEntity);
  if (!entity) {
    return false;
  }
  const char* model = female ? female_skeleton::kPlayerModel : kMaleModel;
  // The running fragments let go of the old character before it goes.
  if (void* controller = body_state::action_controller(actor)) {
    (*reinterpret_cast<Flush**>(controller))[kControllerFlush](controller);
  }
  const int slot = load_character(entity, kBodySlot, model, 0);
  npcs::set_player_model(entity, model);
  on_female[actor] = female;
  reviving = actor;
  revive_frames = 0;
  if (log_line) {
    log_line("APPEARANCE local body on the %s skeleton (%s, slot %d)", female ? "female" : "male",
             model, slot);
  }
  return true;
}

bool keep_figure(void* figure, bool female) {
  if (!figure || !load_character) {
    return false;
  }
  const auto known = on_female.find(figure);
  const bool current = known != on_female.end() && known->second;
  if (current == female) {
    return false;
  }
  void* entity = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(figure) + kActorEntity);
  if (!entity) {
    return false;
  }
  const char* model = female ? female_skeleton::kPlayerModel : kMaleModel;
  // The clothing goes with the old character, while the figure's equipment still counts it as
  // worn, so the game never puts it on the new one: it comes off first and goes on again after.
  std::vector<void*> clothing = outfit::clothing(figure);
  for (void* item : clothing) {
    outfit::wear(figure, item, false);
  }
  if (void* controller = body_state::action_controller(figure)) {
    (*reinterpret_cast<Flush**>(controller))[kControllerFlush](controller);
  }
  const int slot = load_character(entity, kBodySlot, model, 0);
  npcs::set_player_model(entity, model);
  on_female[figure] = female;
  redressing = figure;
  figure_clothing = std::move(clothing);
  redress_frames = 0;
  if (log_line) {
    log_line("APPEARANCE paper-doll figure on the %s skeleton (%s, slot %d), %zu pieces to put on",
             female ? "female" : "male", model, slot, figure_clothing.size());
  }
  return true;
}

bool settle_figure(void* figure) {
  if (!redressing) {
    return false;
  }
  if (redressing != figure) {
    // The screen has another figure now: the old one's clothing is gone with it.
    redressing = nullptr;
    figure_clothing.clear();
    return false;
  }
  if (++redress_frames < kReviveFrames) {
    return false;
  }
  std::vector<void*> refused;
  for (void* item : figure_clothing) {
    if (!outfit::wear(figure, item, true)) {
      refused.push_back(item);
    }
  }
  figure_clothing = std::move(refused);
  if (!figure_clothing.empty() && redress_frames < kRedressFrames) {
    return false;
  }
  if (log_line) {
    log_line("APPEARANCE paper-doll figure dressed on its new skeleton (%zu pieces refused)",
             figure_clothing.size());
  }
  redressing = nullptr;
  figure_clothing.clear();
  return true;
}

bool settle(void* actor) {
  if (!reviving) {
    return false;
  }
  if (reviving != actor) {
    // The player has another actor now (a new world): nothing is left to revive.
    reviving = nullptr;
    return true;
  }
  if (++revive_frames < kReviveFrames) {
    return false;
  }
  reviving = nullptr;
  (*reinterpret_cast<Revive**>(actor))[kActorRevive](actor, kReviveReason);
  if (log_line) {
    log_line("APPEARANCE local body revived on its new skeleton");
  }
  return true;
}

}  // namespace kcds::runtime::appearance::player_skeleton
