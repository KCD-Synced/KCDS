// The containers the server holds each frame. A template over the
// mirror's entities, so it lives here.
#pragma once

#include "../core/client_internal.hpp"
#include "stashes.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace kcds::game_client {

// Containers the server holds: the game's own, where the server put them;
// a knocked-out player's or a dead NPC's at the body as this game shows it (body_at: the body's
// owner, the container to place on it).
template <class Entities, class BodyAt>
void tick_stashes(const Entities& entities, BodyAt body_at) {
  namespace integration = kcd2s::integration;
  std::vector<kcds::runtime::stash::Seen> seen;
  for (const auto& [id, e] : entities) {
    if (e.type != core::type_id("NetworkStash") || e.fields.size() <= integration::StashKeyClass)
      continue;
    const auto position = std::get<core::Vec3>(e.fields[integration::StashPosition]);
    const auto rotation = std::get<core::Quat>(e.fields[integration::StashRotation]);
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
      continue;
    kcds::runtime::stash::Seen container;
    container.id = id;
    const auto body_of = std::get<std::int64_t>(e.fields[integration::StashBodyOf]);
    container.body = body_of != 0;
    container.rotation[0] = rotation.x;
    container.rotation[1] = rotation.y;
    container.rotation[2] = rotation.z;
    container.rotation[3] = rotation.w;
    container.position[0] = position.x;
    container.position[1] = position.y;
    container.position[2] = position.z;
    // The script's lock.
    if (const auto* locked = std::get_if<bool>(&e.fields[integration::StashLocked])) {
      container.lock.locked = *locked && !local_key_bypass.load();
    }
    if (const auto* pickable = std::get_if<bool>(&e.fields[integration::StashLockpickable])) {
      container.lock.canBePicked = *pickable;
    }
    if (const auto* key = std::get_if<std::string>(&e.fields[integration::StashKeyClass])) {
      container.lock.key_class = *key;
    }
    // The script's lock-picking difficulty.
    if (e.fields.size() > integration::StashLockDifficulty) {
      if (const auto* difficulty =
              std::get_if<float>(&e.fields[integration::StashLockDifficulty])) {
        container.lock.difficulty = *difficulty;
      }
    }
    // Which lock the minigame shows.
    if (e.fields.size() > integration::StashLockFanciness) {
      if (const auto* fanciness =
              std::get_if<std::int64_t>(&e.fields[integration::StashLockFanciness])) {
        container.lock.fanciness = int(*fanciness);
      }
    }
    // An adopted level chest.
    if (e.fields.size() > integration::StashLevelGuid) {
      if (const auto* level = std::get_if<std::string>(&e.fields[integration::StashLevelGuid])) {
        container.level_guid = *level;
      }
    }
    // The mesh a script built it as.
    if (e.fields.size() > integration::StashModel) {
      if (const auto* model = std::get_if<std::string>(&e.fields[integration::StashModel])) {
        container.model = *model;
      }
    }
    // Whether its stock holds nothing, for its prompt.
    if (e.fields.size() > integration::StashEmpty) {
      if (const auto* empty = std::get_if<bool>(&e.fields[integration::StashEmpty])) {
        container.empty = *empty;
      }
    }
    if (container.body) {
      body_at(core::EntityId(body_of), container);
    }
    seen.push_back(container);
  }
  kcds::runtime::stash::sync(seen);
}

}  // namespace kcds::game_client
