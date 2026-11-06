#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "kcd2s/core/ids.hpp"

namespace kcd2s::integration {

// One character that already passed the world's visibility and distance rules. Related bodies
// (a mounted player and horse) share a group and are selected together. Higher priority wins;
// distance decides within a priority, with a retention margin for bodies already sent.
struct CharacterCandidate {
  core::EntityId entity = 0;
  core::EntityId group = 0;
  unsigned priority = 0;
  float distance = 0.0f;
};

struct CharacterAdmissionStats {
  std::size_t candidates = 0;
  std::size_t admitted = 0;
  std::size_t resident = 0;
  std::size_t retiring = 0;
  std::size_t deferred = 0;
};

struct CharacterAdmission {
  std::vector<core::EntityId> entities;
  CharacterAdmissionStats stats;
};

// `resident` includes every character possibly delivered to the client, including bodies whose
// DESTRUCT has not been acknowledged and entities already gone from the authoritative world.
// A replacement cannot consume their slots. `viewer` always reserves one slot for the game's
// local player. A zero limit leaves admission disabled; it is not a measured safe capacity.
inline CharacterAdmission admit_characters(const std::vector<CharacterCandidate>& candidates,
                                           const std::unordered_set<core::EntityId>& resident,
                                           core::EntityId viewer, std::size_t limit,
                                           float retention_margin) {
  CharacterAdmission result;
  result.stats.candidates = candidates.size();
  result.stats.resident = resident.size() + (resident.count(viewer) ? 0 : 1);
  if (limit == 0) {
    for (const auto& candidate : candidates) {
      result.entities.push_back(candidate.entity);
    }
    result.stats.admitted = result.entities.size();
    return result;
  }

  struct Group {
    core::EntityId key = 0;
    unsigned priority = 0;
    float distance = std::numeric_limits<float>::infinity();
    bool retained = false;
    bool viewer = false;
    std::vector<core::EntityId> entities;
  };
  std::map<core::EntityId, Group> grouped;
  for (const auto& candidate : candidates) {
    auto& group = grouped[candidate.group ? candidate.group : candidate.entity];
    group.key = candidate.group ? candidate.group : candidate.entity;
    group.priority = std::max(group.priority, candidate.priority);
    if (std::isfinite(candidate.distance)) {
      group.distance = std::min(group.distance, candidate.distance);
    }
    group.retained = group.retained || resident.count(candidate.entity) != 0;
    group.viewer = group.viewer || candidate.entity == viewer;
    group.entities.push_back(candidate.entity);
  }
  std::vector<Group> groups;
  groups.reserve(grouped.size());
  for (auto& [key, group] : grouped) {
    (void)key;
    std::sort(group.entities.begin(), group.entities.end());
    if (group.retained) {
      group.distance -= std::max(0.0f, retention_margin);
    }
    groups.push_back(std::move(group));
  }
  std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
    if (a.viewer != b.viewer) {
      return a.viewer;
    }
    if (a.priority != b.priority) {
      return a.priority > b.priority;
    }
    if (a.distance != b.distance) {
      return a.distance < b.distance;
    }
    return a.key < b.key;
  });

  std::size_t desired_count = 1;
  std::size_t free_slots = limit > result.stats.resident ? limit - result.stats.resident : 0;
  std::unordered_set<core::EntityId> admitted{viewer};
  for (const auto& group : groups) {
    const std::size_t cost = group.entities.size() - (group.viewer ? 1 : 0);
    if (cost > limit - std::min(limit, desired_count)) {
      continue;
    }
    desired_count += cost;
    std::size_t incoming = 0;
    for (const auto entity : group.entities) {
      if (entity != viewer && !resident.count(entity)) {
        ++incoming;
      }
    }
    if (incoming <= free_slots) {
      free_slots -= incoming;
      admitted.insert(group.entities.begin(), group.entities.end());
    } else {
      // Keep the resident part of a desired group while its incoming bodies wait for other
      // groups to leave. This also keeps the local player when its horse is still waiting.
      for (const auto entity : group.entities) {
        if (resident.count(entity)) {
          admitted.insert(entity);
        }
      }
    }
  }
  result.entities.assign(admitted.begin(), admitted.end());
  std::sort(result.entities.begin(), result.entities.end());
  result.stats.admitted = result.entities.size();
  for (const auto entity : resident) {
    if (!admitted.count(entity)) {
      ++result.stats.retiring;
    }
  }
  result.stats.deferred = result.stats.candidates > result.stats.admitted
                              ? result.stats.candidates - result.stats.admitted
                              : 0;
  return result;
}

}  // namespace kcd2s::integration
