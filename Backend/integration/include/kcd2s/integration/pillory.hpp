#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Putting a player into a pillory (KCDS addition Player.putInPillory).
//
//   PILLORY          : server -> a player's own client, on the control channel (0):
//                      [tag][u8 kind][text]. Kind 1 puts the player into the pillory whose holder
//                      entity has the name in the text, kind 2 takes the player out and kind 3
//                      puts the player into the pillory nearest to them in their game (both with
//                      an empty text). The text is the rest of the message: at most
//                      kPilloryNameMax printable ASCII characters, spaces allowed. The server
//                      sends it again when the player's world gets ready: with the name once the
//                      client told it which pillory the nearest was, else kind 3 again.
//   PILLORY_RELEASED : the player's own client -> server, on the control channel (0):
//                      [tag][u8 reason][text]. Reason 1: another player's lockpicking (or a key)
//                      opened the lock of the pillory the client is in, and the client took the
//                      player out. Reason 2: the client is out of the pillory without a lockpick,
//                      because the server ordered it or because the put-in could not be done.
//                      Both have no text. Reason 3: the lock of a pillory was opened in this
//                      client's game, whether or not the player is in a pillory (the lock state
//                      is not synchronised between games, so only the game of the one who picked
//                      it sees it). The text is the lock entity's name, 1 to kPilloryNameMax
//                      printable ASCII characters, and the server lets out every player in the
//                      pillory of that lock. Reason 4: after a kind 3 order the client put the
//                      player into the pillory whose holder entity has the name in the text (the
//                      same bounds), so the server knows which one it was.
//                      The tag is the same in both directions: each side decodes only its own.
namespace kcd2s::integration {

inline constexpr std::uint8_t kPilloryTag = 59;

inline constexpr std::uint8_t kPilloryPutIn = 1;
inline constexpr std::uint8_t kPilloryRelease = 2;
inline constexpr std::uint8_t kPilloryPutInNearest = 3;

inline constexpr std::uint8_t kPilloryLockpicked = 1;
inline constexpr std::uint8_t kPilloryLeft = 2;
inline constexpr std::uint8_t kPilloryLockOpened = 3;
inline constexpr std::uint8_t kPilloryEntered = 4;

// The longest holder entity name. The game's prefab-made names are long: the village pillory's
// holder in Trosecko is "so_pillory[Pillory/pillory1:pillory_prefab[Pillory/pillory1]_<GUID>]"
// (99 characters).
inline constexpr std::size_t kPilloryNameMax = 255;
// Orders a client holds before its game takes them (the oldest go first), and reports a client
// may send per second.
inline constexpr std::size_t kMaxPilloryOrders = 8;
inline constexpr unsigned kMaxPilloryReportsPerSecond = 8;

struct PilloryOrder {
  std::uint8_t kind = kPilloryPutIn;
  std::string holder;  // the holder entity's name; empty for a release and for the nearest

  friend bool operator==(const PilloryOrder& a, const PilloryOrder& b) {
    return a.kind == b.kind && a.holder == b.holder;
  }
};

inline bool pillory_name_valid(const std::string& name) {
  if (name.size() > kPilloryNameMax) {
    return false;
  }
  for (const char c : name) {
    if (c < 0x20 || c > 0x7e) {
      return false;
    }
  }
  return true;
}

// Empty when the order cannot travel: a put-in needs a name, a release and a put-in into the
// nearest pillory have none.
inline std::vector<std::uint8_t> encode_pillory(const PilloryOrder& order) {
  const bool known = order.kind == kPilloryPutIn || order.kind == kPilloryRelease ||
                     order.kind == kPilloryPutInNearest;
  if (!known || !pillory_name_valid(order.holder) ||
      (order.kind == kPilloryPutIn) == order.holder.empty()) {
    return {};
  }
  std::vector<std::uint8_t> out = {kPilloryTag, order.kind};
  out.insert(out.end(), order.holder.begin(), order.holder.end());
  return out;
}

inline bool decode_pillory(const std::vector<std::uint8_t>& p, PilloryOrder& order) {
  if (p.size() < 2 || p[0] != kPilloryTag) {
    return false;
  }
  PilloryOrder decoded;
  decoded.kind = p[1];
  decoded.holder.assign(p.begin() + 2, p.end());
  if (encode_pillory(decoded).empty()) {
    return false;
  }
  order = std::move(decoded);
  return true;
}

// What a client tells the server about a pillory.
struct PilloryReport {
  std::uint8_t reason = kPilloryLeft;
  // The lock entity's name for kPilloryLockOpened, the holder entity's name for kPilloryEntered,
  // otherwise empty.
  std::string lock;

  friend bool operator==(const PilloryReport& a, const PilloryReport& b) {
    return a.reason == b.reason && a.lock == b.lock;
  }
};

// Empty when the report cannot travel: a lock-opened report needs the lock's name and an entered
// report the holder's, the others have none.
inline std::vector<std::uint8_t> encode_pillory_report(const PilloryReport& report) {
  const bool known = report.reason == kPilloryLockpicked || report.reason == kPilloryLeft ||
                     report.reason == kPilloryLockOpened || report.reason == kPilloryEntered;
  const bool named = report.reason == kPilloryLockOpened || report.reason == kPilloryEntered;
  if (!known || !pillory_name_valid(report.lock) || named == report.lock.empty()) {
    return {};
  }
  std::vector<std::uint8_t> out = {kPilloryTag, report.reason};
  out.insert(out.end(), report.lock.begin(), report.lock.end());
  return out;
}

inline bool decode_pillory_report(const std::vector<std::uint8_t>& p, PilloryReport& report) {
  if (p.size() < 2 || p[0] != kPilloryTag) {
    return false;
  }
  PilloryReport decoded;
  decoded.reason = p[1];
  decoded.lock.assign(p.begin() + 2, p.end());
  if (encode_pillory_report(decoded).empty()) {
    return false;
  }
  report = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
