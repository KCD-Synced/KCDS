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

  // Runs every snapshot of every client over every character in view (hundreds in a crowd), so it
  // allocates nothing per group: the members sorted by group and entity, each group a range of
  // them, and only the front of the group order sorted.
  struct Member {
    core::EntityId group = 0;
    core::EntityId entity = 0;
    unsigned priority = 0;
    float distance = 0.0f;
    bool resident = false;
  };
  std::vector<Member> members;
  members.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    members.push_back({candidate.group ? candidate.group : candidate.entity, candidate.entity,
                       candidate.priority, candidate.distance,
                       resident.count(candidate.entity) != 0});
  }
  std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) {
    return a.group != b.group ? a.group < b.group : a.entity < b.entity;
  });
  struct Group {
    core::EntityId key = 0;
    unsigned priority = 0;
    float distance = std::numeric_limits<float>::infinity();
    bool retained = false;
    bool viewer = false;
    std::size_t first = 0;  // into members
    std::size_t count = 0;
  };
  std::vector<Group> groups;
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (groups.empty() || groups.back().key != members[i].group) {
      Group group;
      group.key = members[i].group;
      group.first = i;
      groups.push_back(group);
    }
    auto& group = groups.back();
    ++group.count;
    group.priority = std::max(group.priority, members[i].priority);
    if (std::isfinite(members[i].distance)) {
      group.distance = std::min(group.distance, members[i].distance);
    }
    group.retained = group.retained || members[i].resident;
    group.viewer = group.viewer || members[i].entity == viewer;
  }
  for (auto& group : groups) {
    if (group.retained) {
      group.distance -= std::max(0.0f, retention_margin);
    }
  }
  const auto before = [](const Group& a, const Group& b) {
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
  };
  // Every group but the viewer's alone costs a slot, so the first `limit` groups and the ones a
  // larger group skipped are all the loop below can take; twice the limit is sorted, and all of
  // them when the loop would go past that.
  std::size_t sorted = std::min(groups.size(), 2 * limit + 1);
  std::partial_sort(groups.begin(), groups.begin() + sorted, groups.end(), before);

  std::vector<core::EntityId> admitted{viewer};
  for (bool again = true; again;) {
    again = false;
    admitted.assign(1, viewer);
    std::size_t desired_count = 1;
    std::size_t free_slots = limit > result.stats.resident ? limit - result.stats.resident : 0;
    for (std::size_t g = 0; g < groups.size(); ++g) {
      if (g == sorted) {
        if (desired_count < limit && sorted < groups.size()) {
          // Rare: larger groups were skipped; take the full order and choose again.
          std::sort(groups.begin(), groups.end(), before);
          sorted = groups.size();
          again = true;
        }
        break;
      }
      const auto& group = groups[g];
      const std::size_t cost = group.count - (group.viewer ? 1 : 0);
      if (cost > limit - std::min(limit, desired_count)) {
        continue;
      }
      desired_count += cost;
      std::size_t incoming = 0;
      for (std::size_t m = group.first; m < group.first + group.count; ++m) {
        if (members[m].entity != viewer && !members[m].resident) {
          ++incoming;
        }
      }
      const bool all = incoming <= free_slots;
      if (all) {
        free_slots -= incoming;
      }
      // Keep the resident part of a desired group while its incoming bodies wait for other
      // groups to leave. This also keeps the local player when its horse is still waiting.
      for (std::size_t m = group.first; m < group.first + group.count; ++m) {
        if (all || members[m].resident) {
          admitted.push_back(members[m].entity);
        }
      }
    }
  }
  std::sort(admitted.begin(), admitted.end());
  admitted.erase(std::unique(admitted.begin(), admitted.end()), admitted.end());
  result.entities = std::move(admitted);
  result.stats.admitted = result.entities.size();
  for (const auto entity : resident) {
    if (!std::binary_search(result.entities.begin(), result.entities.end(), entity)) {
      ++result.stats.retiring;
    }
  }
  result.stats.deferred = result.stats.candidates > result.stats.admitted
                              ? result.stats.candidates - result.stats.admitted
                              : 0;
  return result;
}

}  // namespace kcd2s::integration
