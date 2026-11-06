#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "kcd2s/integration/guid.hpp"

// The status effects a server put on a horse (Horse.addBuff): the HorseBuffs field of
// horse.hpp. Every client gives its body of the horse these buffs and keeps them on it; effects the
// game itself put on the horse are not in the field and are left alone.
//
//   field  the buffs' GUIDs in guid_text form, in the order they were added, each followed by ','
//          ("" for none); at most kMaxHorseBuffs, none twice.

namespace kcd2s::integration {

inline constexpr std::size_t kMaxHorseBuffs = 16;

// "" for more than kMaxHorseBuffs or a GUID that is there twice.
inline std::string horse_buffs_field(const std::vector<Guid>& buffs) {
  if (buffs.size() > kMaxHorseBuffs) {
    return {};
  }
  std::string field;
  for (std::size_t i = 0; i < buffs.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      if (buffs[j] == buffs[i]) {
        return {};
      }
    }
    field += guid_text(buffs[i]);
    field += ',';
  }
  return field;
}

// False for anything horse_buffs_field does not write (either case of the digits is taken).
inline bool parse_horse_buffs_field(const std::string& field, std::vector<Guid>& buffs) {
  constexpr std::size_t kEntry = 37;
  if (field.size() % kEntry != 0 || field.size() / kEntry > kMaxHorseBuffs) {
    return false;
  }
  std::vector<Guid> parsed;
  for (std::size_t at = 0; at < field.size(); at += kEntry) {
    Guid id{};
    if (field[at + kEntry - 1] != ',' || !parse_guid(field.substr(at, kEntry - 1), id)) {
      return false;
    }
    for (const auto& known : parsed) {
      if (known == id) {
        return false;
      }
    }
    parsed.push_back(id);
  }
  buffs = std::move(parsed);
  return true;
}

}  // namespace kcd2s::integration
