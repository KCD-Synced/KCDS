#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/ids.hpp"
#include "kcd2s/integration/inventory.hpp"

// Containers the server holds (Stash, a knocked-out player's body).
//
// Each is a NetworkStash entity (local/movement.hpp) that every client in view stands in the
// world as the game's own container: a StashCorpse, which has no model of its own and offers the
// game's own Search. What it holds travels only to the player who opened it:
//
//   - the player uses it: their game waits and reports INVENTORY_REPORT `StashOpen`;
//   - the server grants it (reach, world, nobody else holding it, scripts) and sends STASH_SHOW
//     with `open`: the game fills the container with the rows and opens its own transfer screen;
//   - what the player moves in that screen the game leaves where it is and reports (`StashTake`,
//     `StashPut`); the server moves it, then sends both stocks again;
//   - the screen closes (`StashClose`), or the server ends the hold (STASH_SHOW `closed`).
//
// A locked container that the player opens in their game, with the lock-picking
// minigame or the key, is reported with STASH_UNLOCK before the open that follows, on the same
// channel, so the server has unlocked it when the open arrives.
//
// On the control channel (0).

namespace kcd2s::integration {

inline constexpr std::uint8_t kStashShowTag = 28;
inline constexpr std::uint8_t kStashUnlockTag = 96;
// STASH_UNLOCKs one client may send in a second; more are dropped.
inline constexpr unsigned kMaxStashUnlocksPerSecond = 8;
// Rows one container holds at most (Player.dropAll moves at most this many).
inline constexpr std::size_t kMaxStashRows = 128;

// NetworkStash's fields, in schema order.
enum StashField : core::FieldId {
  StashPosition = 0,  // Vec3: where it stands
  StashRotation,      // Quat
  StashScriptId,      // I64: its id in scripts
  StashBodyOf,        // I64: the network id of the knocked-out player whose body it is, 0
  StashDimension,     // I64
  // The lock a script set (Stash#locked, canBePicked, keyClass), which every client
  // that stands the container applies to the game's own lock.
  StashLocked,        // Bool
  StashLockpickable,  // Bool: the game offers its lock-picking
  StashKeyClass,      // Str: class GUID of its key item, empty when it has no key
  // A level chest the server adopted (Stash.adopt): its level GUID as sixteen hex
  // digits in the level files' order. Every client stands it as that level entity of its own
  // instead of a StashCorpse; empty for any other container.
  StashLevelGuid,  // Str
  // The lock-picking difficulty a script set (Stash#lockLevel), 0 to 1; below 0
  // the game's own.
  StashLockDifficulty,  // F32
  // Which lock the lock-picking minigame shows, as a script set it (Stash#lockStyle, issue):
  // 0 the level's, 1 common, 2 fancy.
  StashLockFanciness,  // I64
  // The mesh a script built the container as (Stash.spawn's model): a .cgf, .cga or
  // .cdf path; empty for the default chest.
  StashModel,  // Str
  // Whether the server's stock of it holds nothing: its prompt says "(empty)" by this, since a
  // client's own inventory of it stays empty until it is opened.
  StashEmpty,  // Bool
};

// The longest model path StashModel carries, in bytes.
inline constexpr std::size_t kMaxStashModelBytes = 255;

// Whether a model can travel in StashModel: empty, or a path of at most kMaxStashModelBytes
// without NUL, backslash or colon that ends in .cgf, .cga or .cdf (any letter case).
inline bool stash_model_valid(const std::string& model) {
  if (model.empty()) {
    return true;
  }
  if (model.size() > kMaxStashModelBytes || model.size() < 5) {
    return false;
  }
  for (const char c : model) {
    if (c == '\0' || c == '\\' || c == ':') {
      return false;
    }
  }
  std::string extension = model.substr(model.size() - 4);
  for (char& c : extension) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return extension == ".cgf" || extension == ".cga" || extension == ".cdf";
}

inline constexpr std::int64_t kLockFancinessLevel = 0;
inline constexpr std::int64_t kLockFancinessCommon = 1;
inline constexpr std::int64_t kLockFancinessFancy = 2;

// Whether a level GUID can travel in StashLevelGuid: empty, or sixteen lowercase hex digits.
inline bool stash_level_guid_valid(const std::string& guid) {
  return guid.empty() ||
         (guid.size() == 16 && guid.find_first_not_of("0123456789abcdef") == std::string::npos);
}

// STASH_SHOW: what a container holds, for the player holding it; or a server horse's saddlebags
// for its owner, whose game shows them in the horse's own inventory.
struct StashShow {
  std::uint32_t stash = 0;  // its network id
  bool open = false;        // open the game's transfer screen on it now
  bool closed = false;      // the hold ended (or was refused): nothing is shown
  std::uint64_t revision = 0;
  std::vector<InventoryRowState> rows;
  std::uint32_t horse = 0;  // the network id of the horse whose saddlebags these are, 0 for none

  friend bool operator==(const StashShow& a, const StashShow& b) {
    return a.stash == b.stash && a.open == b.open && a.closed == b.closed &&
           a.revision == b.revision && a.rows == b.rows && a.horse == b.horse;
  }
};

// STASH_SHOW: [tag][varuint stash][u8 flags: 1 open, 2 closed, 4 a horse's][varuint horse when
// 4][varuint revision][varuint count][rows as in INVENTORY_SHOW]; empty for a show that cannot
// travel.
inline std::vector<std::uint8_t> encode_stash_show(const StashShow& show) {
  if (show.stash == 0 || show.rows.size() > kMaxStashRows || (show.open && show.closed)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kStashShowTag, 8);
  w.write_varuint(show.stash);
  w.write_bits((show.open ? 1 : 0) | (show.closed ? 2 : 0) | (show.horse != 0 ? 4 : 0), 8);
  if (show.horse != 0) {
    w.write_varuint(show.horse);
  }
  w.write_varuint(show.revision);
  w.write_varuint(show.rows.size());
  for (const InventoryRowState& row : show.rows) {
    if (row.id.empty() || row.id.size() > kMaxRowIdBytes) {
      return {};
    }
    inventory_detail::write_row(w, row, true);
  }
  return w.bytes();
}

inline bool decode_stash_show(const std::vector<std::uint8_t>& p, StashShow& show) {
  if (p.empty() || p[0] != kStashShowTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  StashShow decoded;
  const std::uint64_t stash = r.read_varuint();
  const std::uint64_t flags = r.read_bits(8);
  const std::uint64_t horse = (flags & 4) != 0 ? r.read_varuint() : 0;
  decoded.revision = r.read_varuint();
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || stash == 0 || stash > 0xFFFFFFFFu || flags > 6 || (flags & 3) == 3 ||
      ((flags & 4) != 0 && (horse == 0 || horse > 0xFFFFFFFFu)) || count > kMaxStashRows) {
    return false;
  }
  decoded.horse = std::uint32_t(horse);
  decoded.stash = std::uint32_t(stash);
  decoded.open = (flags & 1) != 0;
  decoded.closed = (flags & 2) != 0;
  decoded.rows.resize(count);
  for (InventoryRowState& row : decoded.rows) {
    if (!inventory_detail::read_row(r, row, true)) {
      return false;
    }
  }
  if (!inventory_detail::read_whole(r)) {
    return false;
  }
  show = std::move(decoded);
  return true;
}

// How the player's game opened a container's lock.
enum class StashUnlockMethod : std::uint8_t {
  Pick = 1,  // the lock-picking minigame succeeded
  Key = 2,   // the player holds the key
};

// STASH_UNLOCK: the local player opened a container's lock in their game (client to server).
struct StashUnlock {
  std::uint32_t stash = 0;  // its network id
  StashUnlockMethod method = StashUnlockMethod::Pick;

  friend bool operator==(const StashUnlock& a, const StashUnlock& b) {
    return a.stash == b.stash && a.method == b.method;
  }
};

// The method's name in scripts: "pick" or "key".
inline const char* stash_unlock_method_name(StashUnlockMethod method) {
  return method == StashUnlockMethod::Key ? "key" : "pick";
}

// STASH_UNLOCK: [tag][varuint stash][u8 method]; empty for one that cannot travel.
inline std::vector<std::uint8_t> encode_stash_unlock(const StashUnlock& unlock) {
  if (unlock.stash == 0 ||
      (unlock.method != StashUnlockMethod::Pick && unlock.method != StashUnlockMethod::Key)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kStashUnlockTag, 8);
  w.write_varuint(unlock.stash);
  w.write_bits(std::uint64_t(unlock.method), 8);
  return w.bytes();
}

inline bool decode_stash_unlock(const std::vector<std::uint8_t>& p, StashUnlock& unlock) {
  if (p.empty() || p[0] != kStashUnlockTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t stash = r.read_varuint();
  const std::uint64_t method = r.read_bits(8);
  if (!inventory_detail::read_whole(r) || stash == 0 || stash > 0xFFFFFFFFu ||
      (method != std::uint64_t(StashUnlockMethod::Pick) &&
       method != std::uint64_t(StashUnlockMethod::Key))) {
    return false;
  }
  unlock.stash = std::uint32_t(stash);
  unlock.method = StashUnlockMethod(method);
  return true;
}

}  // namespace kcd2s::integration
