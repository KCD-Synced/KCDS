#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/progression.hpp"

// What a player's replica needs of the player's RPG soul so the game's own combat math on another
// client works with the player's real numbers. A hit is evaluated by the victim's
// game, and its attack value reads the attacker's soul (WHGame 1.5.5: the weapon's
// strength requirement against the strength, and the weapon skill,
// GetSkillLevel); the replica is the attacker's soul there. Before this, every replica
// had strength 0 and the Player class's default skills.
//
// The owner's client reports it in its replicaSoul field (Backend/integration/local/movement.hpp):
// every track's level (kReplicaTracks order) and the perks the player owns directly (the perks
// those grant come with them on the replica, as in the game alone).
namespace kcd2s::integration {

// The tracks a replica takes over: every stat and skill of the game's tables (rpg/stat.xml,
// rpg/skill.xml): the progression's (kTracks, the same order), then the skill rows the progression
// leaves out. A fresh replica had 6 in defense and weapon_dagger while its owner had 5 and 0; the
// shield skill is read when it blocks with a shield.
inline constexpr std::array<TrackId, kTracks.size() + 15> kReplicaTracks = {{
    kTracks[0],  kTracks[1],  kTracks[2],  kTracks[3],  kTracks[4],  kTracks[5],
    kTracks[6],  kTracks[7],  kTracks[8],  kTracks[9],  kTracks[10], kTracks[11],
    kTracks[12], kTracks[13], kTracks[14], kTracks[15], kTracks[16], kTracks[17],
    kTracks[18], kTracks[19], {false, 15},  // defense
    {false, 22},                            // weapon_dagger
    {false, 3},                             // bard
    {false, 7},                             // cooking
    {false, 10},                            // fishing
    {false, 11},                            // mining
    {false, 12},                            // first_aid
    {false, 20},                            // weapon_shield
    {false, 27},                            // tailoring
    {false, 28},                            // armourer
    {false, 29},                            // weaponsmithing
    {false, 30},                            // shoemaking
    {false, 31},                            // gunsmithing
    {false, 32},                            // bowyery
    {false, 33},                            // gambling
}};
static_assert(kTracks.size() == 20);

struct ReplicaSoul {
  std::array<std::uint8_t, kReplicaTracks.size()> levels{};
  std::vector<PerkId> perks;  // owned directly, sorted, no duplicates

  friend bool operator==(const ReplicaSoul& a, const ReplicaSoul& b) {
    return a.levels == b.levels && a.perks == b.perks;
  }
};

// The field's text: the levels, comma separated, then ';' and the perks as the game's tables
// write them (guid_text), comma separated. Empty when it cannot travel (a level past the cap, too
// many perks).
inline std::string replica_soul_field(const ReplicaSoul& soul) {
  if (soul.perks.size() > kMaxPerks) {
    return {};
  }
  std::string field;
  for (std::size_t i = 0; i < soul.levels.size(); ++i) {
    if (soul.levels[i] > kLevelCap) {
      return {};
    }
    if (i > 0) {
      field += ',';
    }
    field += std::to_string(unsigned(soul.levels[i]));
  }
  field += ';';
  for (std::size_t i = 0; i < soul.perks.size(); ++i) {
    if (i > 0) {
      field += ',';
    }
    field += guid_text(soul.perks[i]);
  }
  return field;
}

// False for anything replica_soul_field does not write (an empty field included: the owner has
// not reported yet).
inline bool parse_replica_soul_field(const std::string& field, ReplicaSoul& soul) {
  const std::size_t split = field.find(';');
  if (split == std::string::npos) {
    return false;
  }
  ReplicaSoul parsed;
  std::size_t at = 0;
  for (std::size_t i = 0; i < parsed.levels.size(); ++i) {
    const char end = i + 1 < parsed.levels.size() ? ',' : ';';
    unsigned level = 0;
    std::size_t digits = 0;
    while (at < split && field[at] >= '0' && field[at] <= '9' && digits < 2) {
      level = level * 10 + unsigned(field[at] - '0');
      ++at;
      ++digits;
    }
    if (digits == 0 || level > kLevelCap || at >= field.size() || field[at] != end) {
      return false;
    }
    parsed.levels[i] = std::uint8_t(level);
    ++at;
  }
  if (at != split + 1) {
    return false;
  }
  constexpr std::size_t kPerkText = 36;
  while (at < field.size()) {
    if (parsed.perks.size() == kMaxPerks || field.size() - at < kPerkText) {
      return false;
    }
    PerkId perk{};
    if (!parse_guid(field.substr(at, kPerkText), perk)) {
      return false;
    }
    parsed.perks.push_back(perk);
    at += kPerkText;
    if (at < field.size()) {
      if (field[at] != ',' || at + 1 == field.size()) {
        return false;
      }
      ++at;
    }
  }
  // A list out of order or with duplicates did not come from the owner's report.
  if (!std::is_sorted(parsed.perks.begin(), parsed.perks.end()) ||
      std::adjacent_find(parsed.perks.begin(), parsed.perks.end()) != parsed.perks.end()) {
    return false;
  }
  soul = std::move(parsed);
  return true;
}

}  // namespace kcd2s::integration
