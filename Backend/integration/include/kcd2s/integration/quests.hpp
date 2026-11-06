#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Quests: what a script writes into a player's journal and
// which of them the player follows. The server holds every quest, its progress and its objectives;
// a client only shows them. The server sends a quest to each of its ready players once, again when
// it changes and again when the player's world is ready again; a hide when it is removed or stops
// being theirs; and a track order when the script makes the player follow or stop following it.
// The client reports the tracking it applied, whichever way it happened.
//
//   QUEST_SHOW     : server -> a ready player's client, control channel (0):
//                    [tag][varuint quest][key][title][description][u8 type][u8 progress]
//                    [u8 flags][u8 objective count] then per objective:
//                    [text][u8 progress][u8 objective flags][3 f32 position, when it has one]
//                    key is [varuint length][bytes] of letters, digits, '_' and '-' (1 to 64);
//                    title, description and text are [varuint length][UTF-8 bytes] without
//                    control characters (a description may hold line breaks); flags: 1 announce
//                    (raise the game's quest-updated toast); objective flags: 1 optional,
//                    2 has a position.
//   QUEST_HIDE     : [tag][varuint quest]
//   QUEST_TRACK    : [tag][varuint quest][u8 tracked]  (server -> client: follow it or not)
//   QUEST_TRACKING : [tag][varuint quest][u8 tracked]  (client -> server: the client follows it
//                    now, or no longer; only on a change)
//
// Client/runtime/quests/ shows them.
namespace kcd2s::integration {

inline constexpr std::uint8_t kQuestShowTag = 113;
inline constexpr std::uint8_t kQuestHideTag = 114;
inline constexpr std::uint8_t kQuestTrackTag = 115;
inline constexpr std::uint8_t kQuestTrackingTag = 116;

inline constexpr std::size_t kQuestKeyMax = 64;
inline constexpr std::size_t kQuestTitleMax = 256;
inline constexpr std::size_t kQuestDescriptionMax = 4096;
inline constexpr std::size_t kQuestObjectiveTextMax = 512;
inline constexpr std::size_t kQuestObjectivesMax = 8;
inline constexpr float kQuestCoordinateMax = 100000.0f;
// What a client keeps queued before it applies the orders.
inline constexpr std::size_t kMaxQuestOrders = 256;
// QUEST_TRACKINGs one client may send in a second; more are dropped.
inline constexpr unsigned kMaxQuestTrackingPerSecond = 16;

inline constexpr std::uint8_t kQuestAnnounceBit = 1;
inline constexpr std::uint8_t kObjectiveOptionalBit = 1;
inline constexpr std::uint8_t kObjectivePositionBit = 2;

// The journal section, in the game's own order (E_QuestType).
enum class QuestType : std::uint8_t {
  Main = 0,
  Side = 1,
  Activity = 2,
  Event = 3,
  Micro = 4,
  Racing = 5,
};

// Where a quest or an objective stands.
enum class QuestProgress : std::uint8_t {
  None = 0,
  Active = 1,
  Done = 2,
  Failed = 3,
};

struct QuestObjective {
  std::string text;
  QuestProgress progress = QuestProgress::Active;
  bool optional = false;
  bool has_position = false;
  float position[3] = {0, 0, 0};

  friend bool operator==(const QuestObjective& a, const QuestObjective& b) {
    if (a.text != b.text || a.progress != b.progress || a.optional != b.optional ||
        a.has_position != b.has_position) {
      return false;
    }
    if (!a.has_position) {
      return true;
    }
    return std::memcmp(a.position, b.position, sizeof(a.position)) == 0;
  }
};

struct QuestShow {
  std::uint32_t quest = 0;
  std::string key;
  std::string title;
  std::string description;
  QuestType type = QuestType::Side;
  QuestProgress progress = QuestProgress::Active;
  bool announce = false;
  std::vector<QuestObjective> objectives;

  friend bool operator==(const QuestShow& a, const QuestShow& b) {
    return a.quest == b.quest && a.key == b.key && a.title == b.title &&
           a.description == b.description && a.type == b.type && a.progress == b.progress &&
           a.announce == b.announce && a.objectives == b.objectives;
  }
};

// Track or untrack, either way.
struct QuestTracking {
  std::uint32_t quest = 0;
  bool tracked = false;

  friend bool operator==(const QuestTracking& a, const QuestTracking& b) {
    return a.quest == b.quest && a.tracked == b.tracked;
  }
};

// A show, a hide or a track order of one quest, in the order the server sent them.
struct QuestOrder {
  enum class Kind : std::uint8_t { Show, Hide, Track };
  Kind kind = Kind::Show;
  QuestShow show;  // only `show.quest` is set for a hide or a track order
  bool tracked = false;
};

// The names scripts use.
inline const char* quest_type_name(QuestType type) {
  switch (type) {
    case QuestType::Main:
      return "main";
    case QuestType::Side:
      return "side";
    case QuestType::Activity:
      return "activity";
    case QuestType::Event:
      return "event";
    case QuestType::Micro:
      return "micro";
    case QuestType::Racing:
      return "racing";
  }
  return "side";
}

inline bool quest_type_of(const std::string& name, QuestType& type) {
  static const char* const kNames[] = {"main", "side", "activity", "event", "micro", "racing"};
  for (std::size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
    if (name == kNames[i]) {
      type = QuestType(i);
      return true;
    }
  }
  return false;
}

inline const char* quest_progress_name(QuestProgress progress) {
  switch (progress) {
    case QuestProgress::None:
      return "none";
    case QuestProgress::Active:
      return "active";
    case QuestProgress::Done:
      return "done";
    case QuestProgress::Failed:
      return "failed";
  }
  return "none";
}

inline bool quest_progress_of(const std::string& name, QuestProgress& progress) {
  static const char* const kNames[] = {"none", "active", "done", "failed"};
  for (std::size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
    if (name == kNames[i]) {
      progress = QuestProgress(i);
      return true;
    }
  }
  return false;
}

namespace quest_detail {

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

inline bool key_valid(const std::string& key) {
  if (key.empty() || key.size() > kQuestKeyMax) {
    return false;
  }
  for (const char c : key) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

// UTF-8 text without control characters, at most `max` bytes; `lines` also lets line breaks and
// tabs through.
inline bool text_valid(const std::string& text, std::size_t max, bool lines) {
  if (text.size() > max) {
    return false;
  }
  for (const char c : text) {
    const auto byte = std::uint8_t(c);
    const bool line = c == '\n' || c == '\r' || c == '\t';
    if ((byte < 0x20 && !(lines && line)) || byte == 0x7F) {
      return false;
    }
  }
  return true;
}

inline void write_text(core::BitWriter& w, const std::string& text) {
  w.write_varuint(text.size());
  for (const char c : text) {
    w.write_bits(std::uint8_t(c), 8);
  }
}

inline bool read_text(core::BitReader& r, std::string& text, std::size_t max) {
  const std::uint64_t size = r.read_varuint();
  if (r.overrun() || size > max || size * 8 > r.bits_remaining()) {
    return false;
  }
  text.resize(std::size_t(size));
  for (char& c : text) {
    c = char(r.read_bits(8));
  }
  return !r.overrun();
}

inline bool type_valid(QuestType type) {
  return std::uint8_t(type) <= std::uint8_t(QuestType::Racing);
}

inline bool progress_valid(QuestProgress progress) {
  return std::uint8_t(progress) <= std::uint8_t(QuestProgress::Failed);
}

inline bool position_valid(const float* position) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(position[i]) || std::fabs(position[i]) > kQuestCoordinateMax) {
      return false;
    }
  }
  return true;
}

inline bool valid(const QuestObjective& o) {
  if (!text_valid(o.text, kQuestObjectiveTextMax, false) || !progress_valid(o.progress)) {
    return false;
  }
  return !o.has_position || position_valid(o.position);
}

inline bool valid(const QuestShow& q) {
  if (q.quest == 0 || !key_valid(q.key) || !text_valid(q.title, kQuestTitleMax, false) ||
      !text_valid(q.description, kQuestDescriptionMax, true) || !type_valid(q.type) ||
      !progress_valid(q.progress) || q.objectives.size() > kQuestObjectivesMax) {
    return false;
  }
  for (const auto& objective : q.objectives) {
    if (!valid(objective)) {
      return false;
    }
  }
  return true;
}

inline bool read_whole(const core::BitReader& r) {
  return !r.overrun() && r.bits_remaining() == 0;
}

// [tag][varuint quest][u8 tracked] for QUEST_TRACK and QUEST_TRACKING.
inline std::vector<std::uint8_t> encode_tracking(std::uint8_t tag, const QuestTracking& t) {
  if (t.quest == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(tag, 8);
  w.write_varuint(t.quest);
  w.write_bits(t.tracked ? 1 : 0, 8);
  return w.bytes();
}

inline bool decode_tracking(std::uint8_t tag, const std::vector<std::uint8_t>& p,
                            QuestTracking& t) {
  if (p.empty() || p[0] != tag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t quest = r.read_varuint();
  const std::uint64_t tracked = r.read_bits(8);
  if (!read_whole(r) || quest == 0 || quest > 0xFFFFFFFFu || tracked > 1) {
    return false;
  }
  t.quest = std::uint32_t(quest);
  t.tracked = tracked == 1;
  return true;
}

}  // namespace quest_detail

// Empty for a quest that cannot travel (bad key, texts, enums, positions or too many objectives).
inline std::vector<std::uint8_t> encode_quest_show(const QuestShow& q) {
  if (!quest_detail::valid(q)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kQuestShowTag, 8);
  w.write_varuint(q.quest);
  quest_detail::write_text(w, q.key);
  quest_detail::write_text(w, q.title);
  quest_detail::write_text(w, q.description);
  w.write_bits(std::uint8_t(q.type), 8);
  w.write_bits(std::uint8_t(q.progress), 8);
  w.write_bits(q.announce ? kQuestAnnounceBit : 0, 8);
  w.write_bits(q.objectives.size(), 8);
  for (const auto& objective : q.objectives) {
    quest_detail::write_text(w, objective.text);
    w.write_bits(std::uint8_t(objective.progress), 8);
    std::uint8_t flags = 0;
    if (objective.optional) {
      flags |= kObjectiveOptionalBit;
    }
    if (objective.has_position) {
      flags |= kObjectivePositionBit;
    }
    w.write_bits(flags, 8);
    if (objective.has_position) {
      for (const float value : objective.position) {
        quest_detail::write_float(w, value);
      }
    }
  }
  return w.bytes();
}

inline bool decode_quest_show(const std::vector<std::uint8_t>& p, QuestShow& show) {
  if (p.empty() || p[0] != kQuestShowTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  QuestShow q;
  const std::uint64_t quest = r.read_varuint();
  if (r.overrun() || quest > 0xFFFFFFFFu) {
    return false;
  }
  q.quest = std::uint32_t(quest);
  if (!quest_detail::read_text(r, q.key, kQuestKeyMax) ||
      !quest_detail::read_text(r, q.title, kQuestTitleMax) ||
      !quest_detail::read_text(r, q.description, kQuestDescriptionMax)) {
    return false;
  }
  q.type = QuestType(r.read_bits(8));
  q.progress = QuestProgress(r.read_bits(8));
  const std::uint64_t flags = r.read_bits(8);
  const std::uint64_t count = r.read_bits(8);
  if (r.overrun() || (flags & ~std::uint64_t(kQuestAnnounceBit)) != 0 ||
      count > kQuestObjectivesMax) {
    return false;
  }
  q.announce = (flags & kQuestAnnounceBit) != 0;
  q.objectives.resize(std::size_t(count));
  for (auto& objective : q.objectives) {
    if (!quest_detail::read_text(r, objective.text, kQuestObjectiveTextMax)) {
      return false;
    }
    objective.progress = QuestProgress(r.read_bits(8));
    const std::uint64_t objective_flags = r.read_bits(8);
    if (r.overrun() ||
        (objective_flags & ~std::uint64_t(kObjectiveOptionalBit | kObjectivePositionBit)) != 0) {
      return false;
    }
    objective.optional = (objective_flags & kObjectiveOptionalBit) != 0;
    objective.has_position = (objective_flags & kObjectivePositionBit) != 0;
    if (objective.has_position) {
      for (float& value : objective.position) {
        value = quest_detail::read_float(r);
      }
    }
  }
  if (!quest_detail::read_whole(r) || !quest_detail::valid(q)) {
    return false;
  }
  show = std::move(q);
  return true;
}

inline std::vector<std::uint8_t> encode_quest_hide(std::uint32_t quest) {
  if (quest == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kQuestHideTag, 8);
  w.write_varuint(quest);
  return w.bytes();
}

inline bool decode_quest_hide(const std::vector<std::uint8_t>& p, std::uint32_t& quest) {
  if (p.empty() || p[0] != kQuestHideTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t value = r.read_varuint();
  if (!quest_detail::read_whole(r) || value == 0 || value > 0xFFFFFFFFu) {
    return false;
  }
  quest = std::uint32_t(value);
  return true;
}

// QUEST_TRACK: the server tells the client to follow the quest or to stop.
inline std::vector<std::uint8_t> encode_quest_track(const QuestTracking& track) {
  return quest_detail::encode_tracking(kQuestTrackTag, track);
}

inline bool decode_quest_track(const std::vector<std::uint8_t>& p, QuestTracking& track) {
  return quest_detail::decode_tracking(kQuestTrackTag, p, track);
}

// QUEST_TRACKING: the client reports that it follows the quest now, or no longer.
inline std::vector<std::uint8_t> encode_quest_tracking(const QuestTracking& tracking) {
  return quest_detail::encode_tracking(kQuestTrackingTag, tracking);
}

inline bool decode_quest_tracking(const std::vector<std::uint8_t>& p, QuestTracking& tracking) {
  return quest_detail::decode_tracking(kQuestTrackingTag, p, tracking);
}

}  // namespace kcd2s::integration
