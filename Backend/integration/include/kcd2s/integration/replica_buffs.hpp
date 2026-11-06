#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "kcd2s/integration/guid.hpp"

// The buffs on a player's body that change how it fights, on its replica. A hit is
// evaluated by the victim's game, and its attack value reads the attacker's soul there, the
// replica's, with the modifiers of its buffs (WHGame 1.5.5: strength, the weapon
// skill and the attack modifiers). Without them the attacker's potion, drink or injury changed
// nothing on another player's screen.
//
// The owner's client reports them in its replicaBuffs field
// (Backend/integration/local/movement.hpp): the buff of every instance of a mirrored class, and
// for an injury the game's own numbers of it. Every other client gives its replica's soul the
// same instances through the game's buff manager. The classes the replica's game makes by itself
// (perks, worn items, the body's own state) and those that change health or consciousness, which
// follow the owner by other means, are not mirrored.
namespace kcd2s::integration {

// rpg/buff_class.xml ids: weaponSkill, injury, overeat, alcohol, potion, scriptSystem, hangover,
// satisfaction, perfume, punishment, forcedDrunkenness (Backend/scripting/runtimes/js/buffs/
// catalog.json). Not: system, perk and item (the replica's game makes those), heal, poison, food
// poison, plague and unconsciousness (health and knock-outs follow the owner), perception and the
// test classes.
inline constexpr std::array<std::uint8_t, 11> kReplicaBuffClasses = {2,  5,  9,  10, 13, 15,
                                                                     17, 18, 19, 20, 21};
// More than a body carries of them at once; a report keeps the first ones.
inline constexpr std::size_t kMaxReplicaBuffs = 16;
// An injury's severity is the game's fixed point: this many units are 1 (the whole injury).
inline constexpr std::int32_t kInjurySeverityOne = 32768;
// The report carries an injury's severity in steps of this many units (1/64): the game heals it
// a little every moment, and the report changes only with whole steps.
inline constexpr std::int32_t kInjurySeverityStep = 512;

inline bool replica_buff_class(std::uint32_t buff_class) {
  return std::find(kReplicaBuffClasses.begin(), kReplicaBuffClasses.end(), buff_class) !=
         kReplicaBuffClasses.end();
}

// An injury as the game keeps it (C_InjuredSoulBuffInstance): its type (the injury
// table's id), its two flags (bit 0, bit 1) and its severity (0 .. kInjurySeverityOne; a hit adds
// to it, and the game heals it down over time).
struct InjuryState {
  std::uint32_t type = 0;
  std::uint8_t flags = 0;
  std::int32_t severity = 0;

  friend bool operator==(const InjuryState& a, const InjuryState& b) {
    return a.type == b.type && a.flags == b.flags && a.severity == b.severity;
  }
};

// The drunkenness buff's amount (C_DrunkennessBuff + 248, a float the game saves and copies) is
// carried in thousandths, in steps of kDrunkennessStep, at most kDrunkennessMax. A replica's own
// instance starts without it and ends at once ("does not hold"), so it takes the owner's.
inline constexpr std::int32_t kDrunkennessStep = 100;
inline constexpr std::int32_t kDrunkennessMax = 9999999;

// One mirrored instance: its buff, for an injury the game's numbers of it, and for the
// drunkenness buff its amount.
struct ReplicaBuff {
  Guid buff{};
  std::optional<InjuryState> injury;
  std::optional<std::int32_t> amount;

  friend bool operator==(const ReplicaBuff& a, const ReplicaBuff& b) {
    return a.buff == b.buff && a.injury == b.injury && a.amount == b.amount;
  }
  friend bool operator<(const ReplicaBuff& a, const ReplicaBuff& b) {
    const auto key = [](const ReplicaBuff& entry) {
      const InjuryState injury = entry.injury.value_or(InjuryState{});
      return std::make_tuple(entry.buff, entry.injury.has_value(), injury.type, injury.flags,
                             injury.severity, entry.amount.value_or(-1));
    };
    return key(a) < key(b);
  }
};

namespace detail {

// A whole number of at most `digits` digits at `at`; false for none or a longer one.
inline bool read_number(const std::string& text, std::size_t& at, std::size_t digits,
                        std::int64_t& value) {
  std::size_t read = 0;
  value = 0;
  while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
    if (read == digits) {
      return false;
    }
    value = value * 10 + (text[at] - '0');
    ++at;
    ++read;
  }
  return read > 0;
}

}  // namespace detail

// The field's text: the buffs (guid_text), sorted and comma separated, a buff once per instance;
// an injury's followed by ":type:flags:severity", the drunkenness buff's by "=amount". Empty for
// none; also empty when there are more
// than kMaxReplicaBuffs or an injury's numbers are out of range.
inline std::string replica_buffs_field(std::vector<ReplicaBuff> buffs) {
  if (buffs.size() > kMaxReplicaBuffs) {
    return {};
  }
  std::sort(buffs.begin(), buffs.end());
  std::string field;
  for (std::size_t i = 0; i < buffs.size(); ++i) {
    if (i > 0) {
      field += ',';
    }
    field += guid_text(buffs[i].buff);
    if (buffs[i].injury) {
      const InjuryState& injury = *buffs[i].injury;
      if (injury.flags > 3 || injury.severity < 0 || injury.severity > kInjurySeverityOne ||
          injury.type > 255) {
        return {};
      }
      field += ':' + std::to_string(injury.type) + ':' + std::to_string(unsigned(injury.flags)) +
               ':' + std::to_string(injury.severity);
    } else if (buffs[i].amount) {
      if (*buffs[i].amount < 0 || *buffs[i].amount > kDrunkennessMax) {
        return {};
      }
      field += '=' + std::to_string(*buffs[i].amount);
    }
  }
  return field;
}

// False for anything replica_buffs_field does not write. An empty field is no buffs.
inline bool parse_replica_buffs_field(const std::string& field, std::vector<ReplicaBuff>& buffs) {
  constexpr std::size_t kGuidText = 36;
  std::vector<ReplicaBuff> parsed;
  std::size_t at = 0;
  while (at < field.size()) {
    if (parsed.size() == kMaxReplicaBuffs || field.size() - at < kGuidText) {
      return false;
    }
    ReplicaBuff entry;
    if (!parse_guid(field.substr(at, kGuidText), entry.buff)) {
      return false;
    }
    at += kGuidText;
    if (at < field.size() && field[at] == ':') {
      std::int64_t type = 0;
      std::int64_t flags = 0;
      std::int64_t severity = 0;
      ++at;
      if (!detail::read_number(field, at, 3, type) || type > 255 || at >= field.size() ||
          field[at] != ':') {
        return false;
      }
      ++at;
      if (!detail::read_number(field, at, 1, flags) || flags > 3 || at >= field.size() ||
          field[at] != ':') {
        return false;
      }
      ++at;
      if (!detail::read_number(field, at, 5, severity) || severity > kInjurySeverityOne) {
        return false;
      }
      entry.injury = InjuryState{std::uint32_t(type), std::uint8_t(flags), std::int32_t(severity)};
    } else if (at < field.size() && field[at] == '=') {
      std::int64_t amount = 0;
      ++at;
      if (!detail::read_number(field, at, 7, amount) || amount > kDrunkennessMax) {
        return false;
      }
      entry.amount = std::int32_t(amount);
    }
    parsed.push_back(entry);
    if (at < field.size()) {
      if (field[at] != ',' || at + 1 == field.size()) {
        return false;
      }
      ++at;
    }
  }
  if (!std::is_sorted(parsed.begin(), parsed.end())) {
    return false;
  }
  buffs = std::move(parsed);
  return true;
}

}  // namespace kcd2s::integration
