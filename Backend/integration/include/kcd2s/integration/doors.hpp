#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"

// The level's doors: the game owns the leaf, the server owns the
// state. Every message is on the control channel (0).
//
//   DOOR_SET           : server -> a ready player's client: open or close the door `guid`, from
//                        the front or swung back, and lock or unlock it.
//   DOOR_NEAREST       : server -> a ready player's client: which door is nearest in sight?
//   DOOR_REPORT        : client -> server: the doors near the player and their state, sent when
//                        the world is ready and when a door changes.
//   DOOR_USE           : client -> server: the player's own use of a door, sent before the game
//                        applies it.
//   DOOR_NEAREST_REPLY : client -> server: the answer to a DOOR_NEAREST.
//   DOOR_SPAWN         : server -> a ready player's client in the door's dimension: hang a
//                        door a script made (Door.spawn) or take one down. A hung door
//                        is then a door like the level's: reported, worked and set by its GUID.
//
// Client/runtime/doors/ implements the client side.
namespace kcd2s::integration {

inline constexpr std::uint8_t kDoorSetTag = 71;
inline constexpr std::uint8_t kDoorNearestTag = 72;
inline constexpr std::uint8_t kDoorReportTag = 73;
inline constexpr std::uint8_t kDoorUseTag = 74;
inline constexpr std::uint8_t kDoorNearestReplyTag = 75;
inline constexpr std::uint8_t kDoorSpawnTag = 157;

// Doors in one DOOR_REPORT (a client sends the rest in more messages).
inline constexpr std::size_t kMaxDoorsPerReport = 32;
inline constexpr std::size_t kMaxDoorNameLength = 160;
inline constexpr std::size_t kMaxDoorKeyLength = 64;
inline constexpr std::size_t kMaxDoorReasonLength = 64;
// A door's leaf mesh path, and the short texts of a hung door's properties.
inline constexpr std::size_t kMaxDoorModelLength = 160;
inline constexpr std::size_t kMaxDoorPropertyLength = 64;
// The farthest a door lies from the world's origin (metres) and still travels.
inline constexpr float kMaxDoorCoordinate = 100000.0f;

enum class DoorAction : std::uint8_t { Open = 0, Close = 1, Unlock = 2, Lockpick = 3 };
inline constexpr std::uint8_t kDoorActionCount = 4;

struct DoorState {
  std::uint64_t guid = 0;
  bool open = false;
  bool side = false;  // swung back
  bool locked = false;
  // DOOR_SET only: the lock-picking difficulty a script set (Door#lockLevel), 0
  // to 1; below 0 the level's own. Not part of the door's state as a report compares it.
  float difficulty = -1.0f;
  // DOOR_SET only: the key's item class GUID a script set, "" for the level's own.
  std::string key;
  // DOOR_SET only: the lock kind a script set (Lock.esLockTypes: none/none, manual/none,
  // manual/key or key/key), "" for the door's own.
  std::string lock_types;

  friend bool operator==(const DoorState& a, const DoorState& b) {
    return a.guid == b.guid && a.open == b.open && a.side == b.side && a.locked == b.locked;
  }
};

// A lock-picking difficulty that can travel: 0 to 1, or below 0 for the game's own.
inline bool lock_difficulty_valid(float difficulty) {
  return std::isfinite(difficulty) && difficulty <= 1.0f;
}

struct DoorEntry {
  DoorState state;
  std::string name;
  float position[3] = {0.0f, 0.0f, 0.0f};
  bool canBePicked = false;
  std::string key;  // the key item's class GUID text, or empty
  bool builtLocked = false;
  // Its lock-picking difficulty as the level (or the game) gave it, 0 to 1.
  float difficulty = 0.0f;
  // The leaf mesh it hangs (Properties.object_Model).
  std::string model;

  friend bool operator==(const DoorEntry& a, const DoorEntry& b) {
    return a.state == b.state && a.name == b.name && a.position[0] == b.position[0] &&
           a.position[1] == b.position[1] && a.position[2] == b.position[2] &&
           a.canBePicked == b.canBePicked && a.key == b.key && a.builtLocked == b.builtLocked &&
           a.difficulty == b.difficulty && a.model == b.model;
  }
};

// A door a script hangs (Door.spawn), as the client builds it: an AnimDoor entity of that GUID
// with these properties (Scripts/Entities/Doors/AnimDoor.lua), or, with `remove`, the one of that
// GUID taken down.
struct DoorSpawn {
  std::uint64_t guid = 0;
  bool remove = false;
  std::string model;  // object_Model
  float position[3] = {0.0f, 0.0f, 0.0f};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // x, y, z, w
  std::string anim_set;                          // esDoorAnimSet
  std::string helpers;                           // soclass_SmartObjectHelpers
  std::string smart_object_type;                 // guidSmartObjectType
  bool rigid = false;                            // Physics.bRigidBody
  bool pushable = false;                         // Physics.bPushableByPlayers
  bool locked = false;                           // Lock.bLocked
  std::string lock_types;    // Lock.esLockTypes: none/none, manual/none, manual/key, key/key
  bool canBePicked = true;   // Lock.bCanLockPick
  float difficulty = -1.0f;  // Lock.fLockDifficulty (overridden); below 0 the game's own
  std::string key;           // Lock.guidItemClassId

  friend bool operator==(const DoorSpawn& a, const DoorSpawn& b) {
    return a.guid == b.guid && a.remove == b.remove && a.model == b.model &&
           std::equal(a.position, a.position + 3, b.position) &&
           std::equal(a.rotation, a.rotation + 4, b.rotation) && a.anim_set == b.anim_set &&
           a.helpers == b.helpers && a.smart_object_type == b.smart_object_type &&
           a.rigid == b.rigid && a.pushable == b.pushable && a.locked == b.locked &&
           a.lock_types == b.lock_types && a.canBePicked == b.canBePicked &&
           a.difficulty == b.difficulty && a.key == b.key;
  }
};

struct DoorUse {
  std::uint64_t guid = 0;
  DoorAction action = DoorAction::Open;
  bool keySide = false;  // the player stood on the side the key fits
  // The NPC that worked the door in the sender's game, which simulates it (issue 016: an NPC's
  // brain opens and closes the level's doors); 0 when the player did.
  std::uint32_t npc = 0;

  friend bool operator==(const DoorUse& a, const DoorUse& b) {
    return a.guid == b.guid && a.action == b.action && a.keySide == b.keySide && a.npc == b.npc;
  }
};

struct DoorNearestReply {
  std::uint32_t request = 0;
  bool found = false;
  DoorEntry door;      // when found
  std::string reason;  // when not

  friend bool operator==(const DoorNearestReply& a, const DoorNearestReply& b) {
    return a.request == b.request && a.found == b.found && a.door == b.door && a.reason == b.reason;
  }
};

// A door's GUID as the ops carry it: 16 hex digits.
inline std::string door_guid_text(std::uint64_t guid) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text(16, '0');
  for (int i = 15; i >= 0; --i) {
    text[std::size_t(i)] = kDigits[guid & 0xF];
    guid >>= 4;
  }
  return text;
}

inline bool parse_door_guid(const std::string& text, std::uint64_t& guid) {
  if (text.size() != 16) {
    return false;
  }
  std::uint64_t value = 0;
  for (char c : text) {
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= std::uint64_t(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= std::uint64_t(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value |= std::uint64_t(c - 'A' + 10);
    } else {
      return false;
    }
  }
  guid = value;
  return true;
}

namespace door_detail {

inline void write_text(core::BitWriter& w, const std::string& text) {
  w.write_varuint(text.size());
  w.write_byte_run(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

inline bool read_text(core::BitReader& r, std::string& text, std::size_t longest) {
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || length > longest) {
    return false;
  }
  text.assign(std::size_t(length), '\0');
  r.read_byte_run(reinterpret_cast<std::uint8_t*>(text.data()), text.size());
  return !r.overrun();
}

inline void write_float(core::BitWriter& w, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  w.write_bits(bits, 32);
}

inline float read_float(core::BitReader& r) {
  const std::uint32_t bits = std::uint32_t(r.read_bits(32));
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void write_guid(core::BitWriter& w, std::uint64_t guid) {
  w.write_bits(guid & 0xFFFFFFFFu, 32);
  w.write_bits(guid >> 32, 32);
}

inline std::uint64_t read_guid(core::BitReader& r) {
  const std::uint64_t low = r.read_bits(32);
  const std::uint64_t high = r.read_bits(32);
  return low | (high << 32);
}

inline bool valid(const DoorEntry& door) {
  for (float c : door.position) {
    if (!std::isfinite(c) || std::fabs(c) > kMaxDoorCoordinate) {
      return false;
    }
  }
  return door.state.guid != 0 && door.name.size() <= kMaxDoorNameLength &&
         door.key.size() <= kMaxDoorKeyLength && std::isfinite(door.difficulty) &&
         door.model.size() <= kMaxDoorModelLength;
}

// The longest lock kind text ("manual/none").
inline constexpr std::size_t kMaxLockTypesLength = 11;

// The lock types a hung door takes (AnimDoor.lua esLockTypes, outside/inside).
inline bool valid_lock_types(const std::string& types) {
  return types == "none/none" || types == "manual/none" || types == "manual/key" ||
         types == "key/key";
}

inline bool valid(const DoorSpawn& door) {
  if (door.guid == 0) {
    return false;
  }
  if (door.remove) {
    return true;
  }
  for (float c : door.position) {
    if (!std::isfinite(c) || std::fabs(c) > kMaxDoorCoordinate) {
      return false;
    }
  }
  for (float c : door.rotation) {
    if (!std::isfinite(c) || std::fabs(c) > 1.001f) {
      return false;
    }
  }
  return !door.model.empty() && door.model.size() <= kMaxDoorModelLength &&
         door.anim_set.size() <= kMaxDoorPropertyLength &&
         door.helpers.size() <= kMaxDoorPropertyLength &&
         door.smart_object_type.size() <= kMaxDoorPropertyLength &&
         valid_lock_types(door.lock_types) && door.key.size() <= kMaxDoorKeyLength &&
         lock_difficulty_valid(door.difficulty);
}

inline void write_entry(core::BitWriter& w, const DoorEntry& door) {
  write_guid(w, door.state.guid);
  w.write_bool(door.state.open);
  w.write_bool(door.state.side);
  w.write_bool(door.state.locked);
  w.write_bool(door.canBePicked);
  w.write_bool(door.builtLocked);
  write_text(w, door.name);
  write_text(w, door.key);
  for (float c : door.position) {
    write_float(w, c);
  }
  write_float(w, door.difficulty);
  write_text(w, door.model);
}

inline bool read_entry(core::BitReader& r, DoorEntry& door) {
  DoorEntry decoded;
  decoded.state.guid = read_guid(r);
  decoded.state.open = r.read_bool();
  decoded.state.side = r.read_bool();
  decoded.state.locked = r.read_bool();
  decoded.canBePicked = r.read_bool();
  decoded.builtLocked = r.read_bool();
  if (!read_text(r, decoded.name, kMaxDoorNameLength) ||
      !read_text(r, decoded.key, kMaxDoorKeyLength)) {
    return false;
  }
  for (float& c : decoded.position) {
    c = read_float(r);
  }
  decoded.difficulty = read_float(r);
  if (r.overrun() || !read_text(r, decoded.model, kMaxDoorModelLength) || !valid(decoded)) {
    return false;
  }
  door = std::move(decoded);
  return true;
}

}  // namespace door_detail

// DOOR_SET: [tag][guid u64][open][side][locked][difficulty f32][text key][text lock types];
// empty for a door without a GUID or with a difficulty or lock kind that cannot travel. The lock
// types come last: a client from before them reads the message without them, and a server from
// before them sends none.
inline std::vector<std::uint8_t> encode_door_set(const DoorState& state) {
  if (state.guid == 0 || !lock_difficulty_valid(state.difficulty) ||
      state.key.size() > kMaxDoorKeyLength ||
      (!state.lock_types.empty() && !door_detail::valid_lock_types(state.lock_types))) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kDoorSetTag, 8);
  door_detail::write_guid(w, state.guid);
  w.write_bool(state.open);
  w.write_bool(state.side);
  w.write_bool(state.locked);
  door_detail::write_float(w, state.difficulty < 0.0f ? -1.0f : state.difficulty);
  door_detail::write_text(w, state.key);
  door_detail::write_text(w, state.lock_types);
  return w.bytes();
}

inline bool decode_door_set(const std::vector<std::uint8_t>& p, DoorState& state) {
  if (p.empty() || p[0] != kDoorSetTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DoorState decoded;
  decoded.guid = door_detail::read_guid(r);
  decoded.open = r.read_bool();
  decoded.side = r.read_bool();
  decoded.locked = r.read_bool();
  decoded.difficulty = door_detail::read_float(r);
  if (!door_detail::read_text(r, decoded.key, kMaxDoorKeyLength)) {
    return false;
  }
  // A whole byte left is the lock kind; less is the last byte's padding.
  if (r.bits_remaining() >= 8) {
    if (!door_detail::read_text(r, decoded.lock_types, door_detail::kMaxLockTypesLength) ||
        (!decoded.lock_types.empty() && !door_detail::valid_lock_types(decoded.lock_types))) {
      return false;
    }
  }
  if (r.overrun() || decoded.guid == 0 || !lock_difficulty_valid(decoded.difficulty)) {
    return false;
  }
  state = decoded;
  return true;
}

// DOOR_NEAREST: [tag][request u32].
inline std::vector<std::uint8_t> encode_door_nearest(std::uint32_t request) {
  core::BitWriter w;
  w.write_bits(kDoorNearestTag, 8);
  w.write_bits(request, 32);
  return w.bytes();
}

inline bool decode_door_nearest(const std::vector<std::uint8_t>& p, std::uint32_t& request) {
  if (p.empty() || p[0] != kDoorNearestTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint32_t decoded = std::uint32_t(r.read_bits(32));
  if (r.overrun()) {
    return false;
  }
  request = decoded;
  return true;
}

// DOOR_REPORT: [tag][count varuint][entries]; empty for no doors, too many or a bad entry.
inline std::vector<std::uint8_t> encode_door_report(const std::vector<DoorEntry>& doors) {
  if (doors.empty() || doors.size() > kMaxDoorsPerReport) {
    return {};
  }
  for (const DoorEntry& door : doors) {
    if (!door_detail::valid(door)) {
      return {};
    }
  }
  core::BitWriter w;
  w.write_bits(kDoorReportTag, 8);
  w.write_varuint(doors.size());
  for (const DoorEntry& door : doors) {
    door_detail::write_entry(w, door);
  }
  return w.bytes();
}

inline bool decode_door_report(const std::vector<std::uint8_t>& p, std::vector<DoorEntry>& doors) {
  if (p.empty() || p[0] != kDoorReportTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || count == 0 || count > kMaxDoorsPerReport) {
    return false;
  }
  std::vector<DoorEntry> decoded;
  for (std::uint64_t i = 0; i < count; ++i) {
    DoorEntry door;
    if (!door_detail::read_entry(r, door)) {
      return false;
    }
    decoded.push_back(std::move(door));
  }
  doors = std::move(decoded);
  return true;
}

// DOOR_USE: [tag][guid u64][action u8][keySide].
inline std::vector<std::uint8_t> encode_door_use(const DoorUse& use) {
  if (use.guid == 0 || std::uint8_t(use.action) >= kDoorActionCount) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kDoorUseTag, 8);
  door_detail::write_guid(w, use.guid);
  w.write_bits(std::uint8_t(use.action), 8);
  w.write_bool(use.keySide);
  w.write_varuint(use.npc);
  return w.bytes();
}

inline bool decode_door_use(const std::vector<std::uint8_t>& p, DoorUse& use) {
  if (p.empty() || p[0] != kDoorUseTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DoorUse decoded;
  decoded.guid = door_detail::read_guid(r);
  const std::uint8_t action = std::uint8_t(r.read_bits(8));
  decoded.keySide = r.read_bool();
  const std::uint64_t npc = r.read_varuint();
  if (r.overrun() || decoded.guid == 0 || action >= kDoorActionCount || npc > 0xFFFFFFFFu) {
    return false;
  }
  decoded.action = DoorAction(action);
  decoded.npc = std::uint32_t(npc);
  use = decoded;
  return true;
}

// DOOR_SPAWN: [tag][guid u64][remove]; a door hung also carries [text model][position 3 f32]
// [rotation 4 f32][text anim set][text helpers][text smart object type][rigid][pushable][locked]
// [text lock types][canBePicked][difficulty f32][text key]. Empty for one that cannot travel.
inline std::vector<std::uint8_t> encode_door_spawn(const DoorSpawn& door) {
  using namespace door_detail;
  if (!valid(door)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kDoorSpawnTag, 8);
  write_guid(w, door.guid);
  w.write_bool(door.remove);
  if (door.remove) {
    return w.bytes();
  }
  write_text(w, door.model);
  for (float c : door.position) {
    write_float(w, c);
  }
  for (float c : door.rotation) {
    write_float(w, c);
  }
  write_text(w, door.anim_set);
  write_text(w, door.helpers);
  write_text(w, door.smart_object_type);
  w.write_bool(door.rigid);
  w.write_bool(door.pushable);
  w.write_bool(door.locked);
  write_text(w, door.lock_types);
  w.write_bool(door.canBePicked);
  write_float(w, door.difficulty < 0.0f ? -1.0f : door.difficulty);
  write_text(w, door.key);
  return w.bytes();
}

inline bool decode_door_spawn(const std::vector<std::uint8_t>& p, DoorSpawn& door) {
  using namespace door_detail;
  if (p.empty() || p[0] != kDoorSpawnTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DoorSpawn decoded;
  decoded.guid = read_guid(r);
  decoded.remove = r.read_bool();
  if (!decoded.remove) {
    if (!read_text(r, decoded.model, kMaxDoorModelLength)) {
      return false;
    }
    for (float& c : decoded.position) {
      c = read_float(r);
    }
    for (float& c : decoded.rotation) {
      c = read_float(r);
    }
    if (!read_text(r, decoded.anim_set, kMaxDoorPropertyLength) ||
        !read_text(r, decoded.helpers, kMaxDoorPropertyLength) ||
        !read_text(r, decoded.smart_object_type, kMaxDoorPropertyLength)) {
      return false;
    }
    decoded.rigid = r.read_bool();
    decoded.pushable = r.read_bool();
    decoded.locked = r.read_bool();
    if (!read_text(r, decoded.lock_types, kMaxDoorPropertyLength)) {
      return false;
    }
    decoded.canBePicked = r.read_bool();
    decoded.difficulty = read_float(r);
    if (!read_text(r, decoded.key, kMaxDoorKeyLength)) {
      return false;
    }
  }
  if (r.overrun() || !valid(decoded)) {
    return false;
  }
  door = std::move(decoded);
  return true;
}

// DOOR_NEAREST_REPLY: [tag][request u32][found] then the entry, or the reason.
inline std::vector<std::uint8_t> encode_door_nearest_reply(const DoorNearestReply& reply) {
  if (reply.found ? !door_detail::valid(reply.door) : reply.reason.size() > kMaxDoorReasonLength) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kDoorNearestReplyTag, 8);
  w.write_bits(reply.request, 32);
  w.write_bool(reply.found);
  if (reply.found) {
    door_detail::write_entry(w, reply.door);
  } else {
    door_detail::write_text(w, reply.reason);
  }
  return w.bytes();
}

inline bool decode_door_nearest_reply(const std::vector<std::uint8_t>& p, DoorNearestReply& reply) {
  if (p.empty() || p[0] != kDoorNearestReplyTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DoorNearestReply decoded;
  decoded.request = std::uint32_t(r.read_bits(32));
  decoded.found = r.read_bool();
  if (r.overrun()) {
    return false;
  }
  if (decoded.found ? !door_detail::read_entry(r, decoded.door)
                    : !door_detail::read_text(r, decoded.reason, kMaxDoorReasonLength)) {
    return false;
  }
  reply = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
