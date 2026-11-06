#include "replica_soul.hpp"
#include "progression_report.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace kcds::runtime::progression::replica_soul {
namespace {

namespace integration = kcd2s::integration;

using AddPerk = bool(__fastcall*)(void* perk_set, const integration::PerkId* id,
                                  std::uint64_t source);
using RemovePerk = void(__fastcall*)(void* perk_set, const integration::PerkId* id);

constexpr std::size_t kSkills = 1512;
constexpr std::size_t kStats = 1388;
constexpr std::size_t kPerkSet = 1880;
constexpr std::size_t kPerksBegin = 64;
constexpr std::size_t kPerksEnd = 72;
constexpr std::size_t kPerkId = 16;

Logger log_line = nullptr;
Natives natives;

std::uint8_t* bytes(void* pointer) {
  return static_cast<std::uint8_t*>(pointer);
}

std::int32_t* level_of(void* soul, integration::TrackId track) {
  const std::size_t pair = (track.stat ? kStats : kSkills) + 8 * std::size_t(track.id);
  return reinterpret_cast<std::int32_t*>(bytes(soul) + pair);
}

// The perks the soul owns directly (not the ones those grant), sorted; false for an implausible
// perk set.
bool owned_perks(void* soul, std::vector<integration::PerkId>& perks) {
  std::uint8_t* perk_set = bytes(soul) + kPerkSet;
  void* const* begin = nullptr;
  void* const* end = nullptr;
  std::memcpy(&begin, perk_set + kPerksBegin, sizeof(begin));
  std::memcpy(&end, perk_set + kPerksEnd, sizeof(end));
  if (!begin || end < begin || std::size_t(end - begin) > integration::kMaxPerks) {
    return false;
  }
  perks.clear();
  for (auto it = begin; it != end; ++it) {
    if (!*it) {
      continue;
    }
    integration::PerkId id{};
    std::memcpy(id.data(), static_cast<const std::uint8_t*>(*it) + kPerkId, id.size());
    perks.push_back(id);
  }
  integration::normalize_perks(perks);
  return true;
}

bool contains(const std::vector<integration::PerkId>& sorted, const integration::PerkId& id) {
  return std::binary_search(sorted.begin(), sorted.end(), id);
}

}  // namespace

bool install(const Natives& resolved, Logger logger) {
  natives = resolved;
  log_line = logger;
  return natives.add_perk && natives.remove_perk;
}

bool read(void* soul, integration::ReplicaSoul& soul_state) {
  if (!soul) {
    return false;
  }
  integration::ReplicaSoul read_now;
  for (std::size_t i = 0; i < integration::kReplicaTracks.size(); ++i) {
    const std::int32_t level = *level_of(soul, integration::kReplicaTracks[i]);
    read_now.levels[i] = std::uint8_t(std::clamp<std::int32_t>(level, 0, integration::kLevelCap));
  }
  if (!owned_perks(soul, read_now.perks)) {
    return false;
  }
  if (read_now.perks.size() > integration::kMaxPerks) {
    read_now.perks.resize(integration::kMaxPerks);
  }
  soul_state = std::move(read_now);
  return true;
}

bool apply(void* replica_soul, const integration::ReplicaSoul& owner) {
  if (!replica_soul || !natives.add_perk || !natives.remove_perk) {
    return false;
  }
  for (std::size_t i = 0; i < integration::kReplicaTracks.size(); ++i) {
    *level_of(replica_soul, integration::kReplicaTracks[i]) = owner.levels[i];
  }
  void* perk_set = bytes(replica_soul) + kPerkSet;
  std::vector<integration::PerkId> current;
  if (!owned_perks(replica_soul, current)) {
    return false;
  }
  // The game loads GUIDs with aligned moves: each goes to it from a 16-byte aligned copy.
  alignas(16) integration::PerkId id{};
  for (const integration::PerkId& perk : current) {
    if (!contains(owner.perks, perk)) {
      id = perk;
      reinterpret_cast<RemovePerk>(natives.remove_perk)(perk_set, &id);
    }
  }
  bool complete = true;
  integration::Progression owned;
  for (const integration::PerkId& perk : owner.perks) {
    // A perk an earlier one granted is owned already: the game refuses it, as it would alone.
    if (!progression::read(replica_soul, owned)) {
      return false;
    }
    if (contains(owned.perks, perk)) {
      continue;
    }
    id = perk;
    if (!reinterpret_cast<AddPerk>(natives.add_perk)(perk_set, &id, 0)) {
      complete = false;
      if (log_line) {
        log_line("REPLICA SOUL perk %s refused", integration::perk_id_text(perk).c_str());
      }
    }
  }
  return complete;
}

}  // namespace kcds::runtime::progression::replica_soul
