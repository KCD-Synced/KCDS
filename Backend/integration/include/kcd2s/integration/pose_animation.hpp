#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/integration/physical_prop.hpp"

// A player's pose animation (Asset/Animation Studio): keys of joint turns made in
// the studio, played on the player's body on every client on top of its own animation. The
// server sets it (Player.setAnimation); the NetworkPlayer field `poseAnimation` carries it to
// every client, a player who joins later too, as text, "" for none:
//
//   "A1" <L|N loop> <S|M|I blend: spline, smooth, linear>, then per key "|<time>:" and its
//   joints "<name>=<x>,<y>,<z>" joined by ';' (degrees about the joint's parent's axes)
//
//   "A1LS|0:RightArm=40,0,0;RightForeArm=0,0,0|0.4:RightArm=130,0,-40"
//
// A key may also set the player's physical prop (the studio's prop keys): after its
// joints "@" and "<code><value>" joined by ';', the codes of physical_prop.hpp
// (physical_prop_setting: h hand, p and t grip, g d m a b s c physics):
//
//   "A1LS|0:RightArm=40,0,0@p90;c1|0.4:RightArm=130,0,-40@p20;c2"
//
// A key may also lift the hips (metres up, down below 0: a baked game clip's bent
// knees stand on the ground), "#<lift>" after its joints, before any "@":
//
//   "A1LS|0:LeftLeg=30,0,0#-0.06|0.4:LeftLeg=10,0,0#0"
//
// A key may also give a whip's lash its shape (the studio's Lash to keys): points
// from the handle's tip in the handle's own frame (metres along it, to its right, toward its up
// side), "~<a>,<s>,<u>;..." after the lift, before any "@". While an animation's keys shape the
// lash, the lash takes that shape (blended key to key) instead of physics, on every client alike.
//
//   "A1LS|0:RightArm=40,0,0~0.1,0,0;0.2,0,-0.05|0.4:RightArm=130,0,-40~0.1,0,0.02;0.2,0,0.05"
//
// After the keys an animation may give the lash a track at its own times instead
// (the studio's Lash from motion): "^<rate>,<length>:" (frames a second from the start, the lash's
// metres in equal segments), then its frames joined by '/', each the segments' ways from the
// handle's tip out as two degrees each in the handle's frame, yaw (toward its right from along
// it) then pitch (toward its up side), joined by ','. "^<rate>,<length>,b:" keeps the ways in the
// holding body's frame instead (from its forward toward its right, toward its up): the lash keeps
// its way when the wrist turns. A looping animation's frames go round.
//
//   "A1LS|0:RightArm=40,0,0|0.4:RightArm=130,0,-40^40,1.6:0,-5,3,-8/2,-6,5,-9"
//
// An animation defined by name (Animations.define) may be longer than the field takes
// (kMaxDefinedPoseAnimationBytes): every client gets it once by its name (the reserved client
// event kcds:poseAnimation), and the field names it instead: "R1" <L|N loop> <name>.
//
// Each client plays it from when it got it (the start is not shared) and turns the joints the
// body has by name. turns_at() blends the keys as the studio's editor does
// (Client/resources/studio/animation.cjs): a Catmull-Rom curve on the degrees for spline, eased
// or even for smooth and linear. pose_prop_at() gives the prop's settings at a time.
namespace kcd2s::integration {

inline constexpr std::size_t kMaxPoseAnimationBytes = 2000;
inline constexpr std::size_t kMaxDefinedPoseAnimationBytes = 30000;
inline constexpr std::size_t kMaxPoseAnimationKeys = 32;
inline constexpr std::size_t kMaxPoseAnimationJoints = 32;
inline constexpr float kLongestPoseAnimation = 60.0f;
inline constexpr std::size_t kLongestJointName = 48;
// How far a key lifts the hips (metres).
inline constexpr float kLowestLift = -1.0f;
inline constexpr float kHighestLift = 0.5f;
// A key's lash shape: at most this many points, each this far from the handle at most (metres).
inline constexpr std::size_t kMostLashPoints = 16;
inline constexpr float kFarthestLashPoint = 6.0f;
// A lash track: frames a second, the lash's length (metres), frames, segments in each.
inline constexpr float kLowestLashTrackRate = 1.0f;
inline constexpr float kHighestLashTrackRate = 60.0f;
inline constexpr float kShortestLashTrack = 0.1f;
inline constexpr float kLongestLashTrack = 6.0f;
inline constexpr std::size_t kMostLashTrackFrames = 600;
inline constexpr std::size_t kFewestLashTrackSegments = 2;
inline constexpr std::size_t kMostLashTrackSegments = 16;

enum class PoseBlend { Spline, Smooth, Linear };

struct PoseKey {
  float time = 0.0f;
  // Joint name to its turn, degrees x, y, z.
  std::map<std::string, std::array<float, 3>> turns;
  // The prop's settings this key sets, by code (physical_prop.hpp physical_prop_setting).
  std::map<char, float> prop;
  // How far it lifts the hips (metres), when it says.
  std::optional<float> lift;
  // The whip's lash shape it gives: points in the handle's frame (along, right, up), empty for
  // none.
  std::vector<std::array<float, 3>> lash;
};

// A whip's lash at its own times: each frame its segments' [yaw, pitch, yaw, pitch, ...]
// degrees in the handle's frame; no frames for none.
struct PoseLashTrack {
  float rate = 0.0f;
  float length = 0.0f;
  std::vector<std::vector<float>> frames;
  // The ways in the holding body's frame (forward, right, up: ",b" in the field) instead of the
  // handle's: the lash keeps its way when the wrist turns.
  bool body = false;
};

struct PoseAnimation {
  bool loop = true;
  PoseBlend blend = PoseBlend::Spline;
  std::vector<PoseKey> keys;
  PoseLashTrack lash_track;

  float length() const {
    return keys.empty() ? 0.0f : keys.back().time;
  }
};

inline bool pose_joint_name(const std::string& name) {
  return !name.empty() && name.size() <= kLongestJointName &&
         std::all_of(name.begin(), name.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_';
         });
}

// Whether a lash track is one: its rate and length in range, 2 to kMostLashTrackFrames frames of
// the same 2 to kMostLashTrackSegments segments, yaws within ±180° and pitches within ±90°.
inline bool pose_lash_track_good(const PoseLashTrack& track) {
  if (!(track.rate >= kLowestLashTrackRate && track.rate <= kHighestLashTrackRate) ||
      !(track.length >= kShortestLashTrack && track.length <= kLongestLashTrack) ||
      track.frames.size() < 2 || track.frames.size() > kMostLashTrackFrames) {
    return false;
  }
  const std::size_t numbers = track.frames.front().size();
  if (numbers % 2 != 0 || numbers / 2 < kFewestLashTrackSegments ||
      numbers / 2 > kMostLashTrackSegments) {
    return false;
  }
  for (const auto& frame : track.frames) {
    if (frame.size() != numbers) {
      return false;
    }
    for (std::size_t index = 0; index < frame.size(); ++index) {
      const float limit = index % 2 == 0 ? 180.0f : 90.0f;
      if (!std::isfinite(frame[index]) || std::fabs(frame[index]) > limit) {
        return false;
      }
    }
  }
  return true;
}

// The lash track as the field writes it ("^<rate>,<length>:" and its frames); "" for none or a
// bad one.
inline std::string pose_lash_track_field(const PoseLashTrack& track) {
  if (!pose_lash_track_good(track)) {
    return {};
  }
  char number[64];
  std::snprintf(number, sizeof(number), "^%g,%g%s:", double(track.rate), double(track.length),
                track.body ? ",b" : "");
  std::string text = number;
  for (std::size_t frame = 0; frame < track.frames.size(); ++frame) {
    if (frame > 0) {
      text += '/';
    }
    for (std::size_t index = 0; index < track.frames[frame].size(); ++index) {
      std::snprintf(number, sizeof(number), "%s%g", index == 0 ? "" : ",",
                    double(track.frames[frame][index]));
      text += number;
    }
  }
  return text;
}

// A lash track read from the field's text after its '^'; nothing when it is not one.
inline std::optional<PoseLashTrack> pose_lash_track_of(const std::string& text) {
  PoseLashTrack track;
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  std::string head = text.substr(0, colon);
  // ",b" at the end: the ways are in the holding body's frame.
  if (head.size() > 2 && head.compare(head.size() - 2, 2, ",b") == 0) {
    track.body = true;
    head.resize(head.size() - 2);
  }
  char tail = '\0';
  if (std::sscanf(head.c_str(), "%f,%f%c", &track.rate, &track.length, &tail) != 2) {
    return std::nullopt;
  }
  std::size_t at = colon + 1;
  while (at <= text.size()) {
    std::size_t stop = text.find('/', at);
    if (stop == std::string::npos) {
      stop = text.size();
    }
    std::vector<float> frame;
    std::size_t entry = at;
    while (entry < stop) {
      std::size_t comma = text.find(',', entry);
      if (comma == std::string::npos || comma > stop) {
        comma = stop;
      }
      const std::string item = text.substr(entry, comma - entry);
      char* after = nullptr;
      const float value = std::strtof(item.c_str(), &after);
      if (item.empty() || *after != '\0') {
        return std::nullopt;
      }
      frame.push_back(value);
      entry = comma + 1;
    }
    track.frames.push_back(std::move(frame));
    if (stop == text.size()) {
      break;
    }
    at = stop + 1;
  }
  if (!pose_lash_track_good(track)) {
    return std::nullopt;
  }
  return track;
}

// The animation as the field's text; "" when it does not fit (longer than `most` bytes, too many
// keys or joints, times not rising from 0, a bad name, turn or lift).
inline std::string pose_animation_field(const PoseAnimation& animation,
                                        std::size_t most = kMaxPoseAnimationBytes) {
  if (animation.keys.empty() || animation.keys.size() > kMaxPoseAnimationKeys ||
      animation.keys.front().time != 0.0f) {
    return {};
  }
  std::string text = "A1";
  text += animation.loop ? 'L' : 'N';
  text += animation.blend == PoseBlend::Spline   ? 'S'
          : animation.blend == PoseBlend::Smooth ? 'M'
                                                 : 'I';
  float previous = -1.0f;
  for (const PoseKey& key : animation.keys) {
    if (!(key.time > previous) || key.time > kLongestPoseAnimation ||
        key.turns.size() > kMaxPoseAnimationJoints) {
      return {};
    }
    previous = key.time;
    char number[64];
    std::snprintf(number, sizeof(number), "|%g:", double(key.time));
    text += number;
    bool first = true;
    for (const auto& [name, turn] : key.turns) {
      if (!pose_joint_name(name) || !std::isfinite(turn[0]) || !std::isfinite(turn[1]) ||
          !std::isfinite(turn[2])) {
        return {};
      }
      std::snprintf(number, sizeof(number), "%s%s=%g,%g,%g", first ? "" : ";", name.c_str(),
                    double(turn[0]), double(turn[1]), double(turn[2]));
      text += number;
      first = false;
    }
    if (key.lift) {
      if (!std::isfinite(*key.lift) || *key.lift < kLowestLift || *key.lift > kHighestLift) {
        return {};
      }
      std::snprintf(number, sizeof(number), "#%g", double(*key.lift));
      text += number;
    }
    if (key.lash.size() > kMostLashPoints) {
      return {};
    }
    for (std::size_t index = 0; index < key.lash.size(); ++index) {
      const auto& point = key.lash[index];
      for (const float part : point) {
        if (!std::isfinite(part) || std::fabs(part) > kFarthestLashPoint) {
          return {};
        }
      }
      std::snprintf(number, sizeof(number), "%s%g,%g,%g", index == 0 ? "~" : ";", double(point[0]),
                    double(point[1]), double(point[2]));
      text += number;
    }
    bool first_setting = true;
    for (const auto& [code, value] : key.prop) {
      const PhysicalPropSetting* setting = physical_prop_setting(code);
      if (!setting || !std::isfinite(value) || value < setting->low || value > setting->high) {
        return {};
      }
      std::snprintf(number, sizeof(number), "%s%c%g", first_setting ? "@" : ";", code,
                    double(value));
      text += number;
      first_setting = false;
    }
  }
  if (!animation.lash_track.frames.empty()) {
    const std::string track = pose_lash_track_field(animation.lash_track);
    if (track.empty()) {
      return {};
    }
    text += track;
  }
  return text.size() <= most ? text : std::string();
}

// The field's text read back (at most `most` bytes); nothing when it is not one.
inline std::optional<PoseAnimation> pose_animation_of(const std::string& field,
                                                      std::size_t most = kMaxPoseAnimationBytes) {
  if (field.size() < 4 || field.size() > most || field.compare(0, 2, "A1") != 0) {
    return std::nullopt;
  }
  // A lash track after the keys: the keys read alone, then the track.
  const std::size_t caret = field.find('^');
  if (caret != std::string::npos) {
    auto animation = pose_animation_of(field.substr(0, caret), most);
    auto track = pose_lash_track_of(field.substr(caret + 1));
    if (!animation || !track) {
      return std::nullopt;
    }
    animation->lash_track = std::move(*track);
    if (pose_animation_field(*animation, most).empty()) {
      return std::nullopt;
    }
    return animation;
  }
  PoseAnimation animation;
  if (field[2] != 'L' && field[2] != 'N') {
    return std::nullopt;
  }
  animation.loop = field[2] == 'L';
  switch (field[3]) {
    case 'S':
      animation.blend = PoseBlend::Spline;
      break;
    case 'M':
      animation.blend = PoseBlend::Smooth;
      break;
    case 'I':
      animation.blend = PoseBlend::Linear;
      break;
    default:
      return std::nullopt;
  }
  std::size_t at = 4;
  while (at < field.size()) {
    if (field[at] != '|') {
      return std::nullopt;
    }
    const std::size_t colon = field.find(':', at);
    if (colon == std::string::npos) {
      return std::nullopt;
    }
    PoseKey key;
    char* end = nullptr;
    const std::string time_text = field.substr(at + 1, colon - at - 1);
    key.time = std::strtof(time_text.c_str(), &end);
    if (time_text.empty() || *end != '\0') {
      return std::nullopt;
    }
    std::size_t next = field.find('|', colon);
    if (next == std::string::npos) {
      next = field.size();
    }
    // The prop's settings after '@', then the joints before it.
    std::size_t joints_end = next;
    const std::size_t settings = field.find('@', colon);
    if (settings != std::string::npos && settings < next) {
      std::size_t entry = settings + 1;
      while (entry < next) {
        std::size_t stop = field.find(';', entry);
        if (stop == std::string::npos || stop > next) {
          stop = next;
        }
        const std::string item = field.substr(entry, stop - entry);
        char* after = nullptr;
        const float value = item.size() >= 2 ? std::strtof(item.c_str() + 1, &after) : 0.0f;
        if (item.size() < 2 || *after != '\0' || !physical_prop_setting(item[0]) ||
            key.prop.count(item[0]) != 0) {
          return std::nullopt;
        }
        key.prop[item[0]] = value;
        entry = stop + 1;
      }
      joints_end = settings;
    }
    // The lash's shape after '~', before the settings.
    const std::size_t lash = field.find('~', colon);
    if (lash != std::string::npos && lash < joints_end) {
      std::size_t entry = lash + 1;
      while (entry < joints_end) {
        std::size_t stop = field.find(';', entry);
        if (stop == std::string::npos || stop > joints_end) {
          stop = joints_end;
        }
        const std::string item = field.substr(entry, stop - entry);
        std::array<float, 3> point{};
        char tail = '\0';
        if (std::sscanf(item.c_str(), "%f,%f,%f%c", &point[0], &point[1], &point[2], &tail) != 3) {
          return std::nullopt;
        }
        key.lash.push_back(point);
        entry = stop + 1;
      }
      joints_end = lash;
    }
    // The hips' lift after '#', before the lash and the settings.
    const std::size_t lift = field.find('#', colon);
    if (lift != std::string::npos && lift < joints_end) {
      const std::string lift_text = field.substr(lift + 1, joints_end - lift - 1);
      char* after = nullptr;
      const float value = std::strtof(lift_text.c_str(), &after);
      if (lift_text.empty() || *after != '\0') {
        return std::nullopt;
      }
      key.lift = value;
      joints_end = lift;
    }
    std::size_t part = colon + 1;
    while (part < joints_end) {
      std::size_t stop = field.find(';', part);
      if (stop == std::string::npos || stop > joints_end) {
        stop = joints_end;
      }
      const std::string joint = field.substr(part, stop - part);
      const std::size_t equals = joint.find('=');
      if (equals == std::string::npos) {
        return std::nullopt;
      }
      std::array<float, 3> turn{};
      const std::string name = joint.substr(0, equals);
      if (!pose_joint_name(name) ||
          std::sscanf(joint.c_str() + equals + 1, "%f,%f,%f", &turn[0], &turn[1], &turn[2]) != 3) {
        return std::nullopt;
      }
      key.turns[name] = turn;
      part = stop + 1;
    }
    animation.keys.push_back(std::move(key));
    at = next;
  }
  // Read back, it must be what the field would say.
  if (pose_animation_field(animation, most).empty()) {
    return std::nullopt;
  }
  return animation;
}

// A field that names an animation defined by name ("R1" <L|N loop> <name>): its name and loop;
// false for another field.
inline bool pose_animation_reference(const std::string& field, std::string& name, bool& loop) {
  if (field.size() < 4 || field.compare(0, 2, "R1") != 0 || (field[2] != 'L' && field[2] != 'N')) {
    return false;
  }
  const std::string given = field.substr(3);
  if (!pose_joint_name(given)) {
    return false;
  }
  name = given;
  loop = field[2] == 'L';
  return true;
}

// A segment's way from its yaw and pitch (degrees) in the handle's frame (along, right, up).
inline std::array<float, 3> pose_lash_way(float yaw, float pitch) {
  constexpr float kRadians = 3.14159265358979f / 180.0f;
  const float y = yaw * kRadians;
  const float p = pitch * kRadians;
  return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}

// The way between two unit ways at `share`, a unit way again (the first when they are opposite).
inline std::array<float, 3> pose_way_between(const std::array<float, 3>& a,
                                             const std::array<float, 3>& b, float share) {
  std::array<float, 3> mixed{};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    mixed[axis] = a[axis] + (b[axis] - a[axis]) * share;
  }
  const float length = std::sqrt(mixed[0] * mixed[0] + mixed[1] * mixed[1] + mixed[2] * mixed[2]);
  if (length <= 1e-6f) {
    return a;
  }
  for (float& part : mixed) {
    part /= length;
  }
  return mixed;
}

// The lash's points at `time` from its track (as the studio's editor plays it,
// Client/resources/studio/animation.cjs lashTrackAt): the two frames around it, each segment's way
// blended, the segments chained from the handle's tip: always its whole length.
inline std::vector<std::array<float, 3>> pose_lash_track_at(const PoseLashTrack& track, bool loop,
                                                            float time) {
  const std::size_t count = track.frames.size();
  const float at = std::max(0.0f, time) * track.rate;
  std::size_t first = std::size_t(std::floor(at));
  std::size_t second = first + 1;
  float share = at - std::floor(at);
  if (loop) {
    first %= count;
    second %= count;
  } else if (first >= count - 1) {
    first = count - 1;
    second = count - 1;
    share = 0.0f;
  }
  const auto& a = track.frames[first];
  const auto& b = track.frames[second];
  const std::size_t segments = a.size() / 2;
  const float piece = track.length / float(segments);
  std::vector<std::array<float, 3>> points;
  std::array<float, 3> point{};
  for (std::size_t segment = 0; segment < segments; ++segment) {
    const auto from = pose_lash_way(a[segment * 2], a[segment * 2 + 1]);
    const auto to = pose_lash_way(b[segment * 2], b[segment * 2 + 1]);
    const auto way = pose_way_between(from, to, share);
    for (std::size_t axis = 0; axis < 3; ++axis) {
      point[axis] += way[axis] * piece;
    }
    points.push_back(point);
  }
  return points;
}

// Two keys' lash shapes blended at `share` piece by piece: each piece's way blended and its length
// between the two, chained from the handle's tip (as animation.cjs lashBetween; blending the points
// themselves drew a bent lash shorter between two keys).
inline std::vector<std::array<float, 3>> pose_lash_between(
    const std::vector<std::array<float, 3>>& from, const std::vector<std::array<float, 3>>& to,
    float share) {
  std::vector<std::array<float, 3>> points;
  std::array<float, 3> point{};
  std::array<float, 3> from_before{};
  std::array<float, 3> to_before{};
  for (std::size_t index = 0; index < from.size(); ++index) {
    std::array<float, 3> a{};
    std::array<float, 3> b{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      a[axis] = from[index][axis] - from_before[axis];
      b[axis] = to[index][axis] - to_before[axis];
    }
    from_before = from[index];
    to_before = to[index];
    const float a_length = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float b_length = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    std::array<float, 3> a_way = {1.0f, 0.0f, 0.0f};
    if (a_length > 1e-6f) {
      a_way = {a[0] / a_length, a[1] / a_length, a[2] / a_length};
    }
    std::array<float, 3> b_way = a_way;
    if (b_length > 1e-6f) {
      b_way = {b[0] / b_length, b[1] / b_length, b[2] / b_length};
    }
    const auto way = pose_way_between(a_way, b_way, share);
    const float length = a_length + (b_length - a_length) * share;
    for (std::size_t axis = 0; axis < 3; ++axis) {
      point[axis] += way[axis] * length;
    }
    points.push_back(point);
  }
  return points;
}

// The lash's shape at `time`: from the animation's lash track when it has one, else between the
// keys that shape it, piece by piece (pose_lash_between); empty when nothing does.
inline std::vector<std::array<float, 3>> pose_lash_at(const PoseAnimation& animation, float time) {
  if (!animation.lash_track.frames.empty()) {
    return pose_lash_track_at(animation.lash_track, animation.loop, time);
  }
  const PoseKey* before = nullptr;
  const PoseKey* after = nullptr;
  for (const PoseKey& key : animation.keys) {
    if (key.lash.empty()) {
      continue;
    }
    if (key.time <= time) {
      before = &key;
    } else if (!after) {
      after = &key;
    }
  }
  if (!before && !after) {
    return {};
  }
  if (!before || !after || before->lash.size() != after->lash.size()) {
    return before ? before->lash : after->lash;
  }
  const float share = (time - before->time) / (after->time - before->time);
  return pose_lash_between(before->lash, after->lash, share);
}

// How far the hips are lifted at `time` (metres): between the keys that lift them, as the keys
// blend (eased or even; a spline is eased); 0 when none does.
inline float pose_lift_at(const PoseAnimation& animation, float time) {
  const PoseKey* before = nullptr;
  const PoseKey* after = nullptr;
  for (const PoseKey& key : animation.keys) {
    if (!key.lift) {
      continue;
    }
    if (key.time <= time) {
      before = &key;
    } else if (!after) {
      after = &key;
    }
  }
  if (!before && !after) {
    return 0.0f;
  }
  if (!before) {
    return *after->lift;
  }
  if (!after) {
    return *before->lift;
  }
  const float linear = (time - before->time) / (after->time - before->time);
  const float share =
      animation.blend == PoseBlend::Linear ? linear : linear * linear * (3 - 2 * linear);
  return *before->lift + (*after->lift - *before->lift) * share;
}

// Where playing is `elapsed` seconds after it started: round again when it loops, held at the
// end when it does not.
inline float pose_play_time(const PoseAnimation& animation, double elapsed) {
  const float length = animation.length();
  if (length <= 0.0f || elapsed <= 0.0) {
    return 0.0f;
  }
  if (animation.loop) {
    return float(std::fmod(elapsed, double(length)));
  }
  return float(std::min(elapsed, double(length)));
}

// Every turned joint's turn in degrees at `time`.
inline std::map<std::string, std::array<float, 3>> pose_turns_at(const PoseAnimation& animation,
                                                                 float time) {
  const auto& keys = animation.keys;
  if (keys.empty()) {
    return {};
  }
  std::size_t next = 0;
  while (next < keys.size() && keys[next].time <= time) {
    ++next;
  }
  if (next == 0) {
    return keys.front().turns;
  }
  if (next == keys.size()) {
    return keys.back().turns;
  }
  const PoseKey& from = keys[next - 1];
  const PoseKey& to = keys[next];
  const float linear = (time - from.time) / (to.time - from.time);
  std::map<std::string, std::array<float, 3>> turns;
  auto turn_in = [](const PoseKey* key, const std::string& joint) {
    if (!key) {
      return std::array<float, 3>{0, 0, 0};
    }
    const auto found = key->turns.find(joint);
    return found == key->turns.end() ? std::array<float, 3>{0, 0, 0} : found->second;
  };
  if (animation.blend != PoseBlend::Spline) {
    const float share =
        animation.blend == PoseBlend::Smooth ? linear * linear * (3 - 2 * linear) : linear;
    for (const PoseKey* key : {&from, &to}) {
      for (const auto& [joint, unused] : key->turns) {
        const auto a = turn_in(&from, joint);
        const auto b = turn_in(&to, joint);
        turns[joint] = {a[0] + (b[0] - a[0]) * share, a[1] + (b[1] - a[1]) * share,
                        a[2] + (b[2] - a[2]) * share};
      }
    }
    return turns;
  }
  // The keys round the segment, looping round when the animation loops (its last key is its
  // first one again), with their times on the same line.
  const std::size_t index = next - 1;
  const float length = animation.length();
  auto neighbour = [&](long offset, float& at) -> const PoseKey* {
    const long wanted = long(index) + offset;
    const long count = long(keys.size());
    if (wanted >= 0 && wanted < count) {
      at = keys[std::size_t(wanted)].time;
      return &keys[std::size_t(wanted)];
    }
    if (!animation.loop || count < 3) {
      return nullptr;
    }
    if (wanted < 0) {
      const PoseKey& key = keys[std::size_t(count - 1 + wanted)];
      at = key.time - length;
      return &key;
    }
    const PoseKey& key = keys[std::size_t(wanted - count + 1)];
    at = key.time + length;
    return &key;
  };
  float t0 = 0;
  float t1 = 0;
  float t2 = 0;
  float t3 = 0;
  const PoseKey* k0 = neighbour(-1, t0);
  const PoseKey* k1 = neighbour(0, t1);
  const PoseKey* k2 = neighbour(1, t2);
  const PoseKey* k3 = neighbour(2, t3);
  const float span = t2 - t1;
  const float s = linear;
  const float h00 = 2 * s * s * s - 3 * s * s + 1;
  const float h10 = s * s * s - 2 * s * s + s;
  const float h01 = -2 * s * s * s + 3 * s * s;
  const float h11 = s * s * s - s * s;
  for (const PoseKey* key : {k0, k1, k2, k3}) {
    if (!key) {
      continue;
    }
    for (const auto& [joint, unused] : key->turns) {
      if (turns.count(joint) != 0) {
        continue;
      }
      std::array<float, 3> turn{};
      for (int axis = 0; axis < 3; ++axis) {
        const float p1 = turn_in(k1, joint)[axis];
        const float p2 = turn_in(k2, joint)[axis];
        const float m1 =
            k0 && k2 ? (turn_in(k2, joint)[axis] - turn_in(k0, joint)[axis]) / (t2 - t0) * span
                     : 0.0f;
        const float m2 =
            k1 && k3 ? (turn_in(k3, joint)[axis] - turn_in(k1, joint)[axis]) / (t3 - t1) * span
                     : 0.0f;
        turn[axis] = h00 * p1 + h10 * m1 + h01 * p2 + h11 * m2;
      }
      turns[joint] = turn;
    }
  }
  return turns;
}

// The prop's settings the keys set, at `time`: each code from the key at or before
// it that sets that code toward the next key that sets it (eased for spline and smooth, even for
// linear); a stepped one (the hand, collide) and one with no next key keep the earlier key's
// value; before the first key that sets a code, that key's value. Empty for no prop keys.
inline std::map<char, float> pose_prop_at(const PoseAnimation& animation, float time) {
  std::map<char, float> values;
  for (const char* code = kPhysicalPropCodes; *code; ++code) {
    const PoseKey* before = nullptr;
    const PoseKey* after = nullptr;
    for (const PoseKey& key : animation.keys) {
      if (key.prop.count(*code) == 0) {
        continue;
      }
      if (key.time <= time) {
        before = &key;
      } else if (!after) {
        after = &key;
      }
    }
    if (!before && !after) {
      continue;
    }
    if (!before) {
      values[*code] = after->prop.at(*code);
      continue;
    }
    const float from = before->prop.at(*code);
    const PhysicalPropSetting* setting = physical_prop_setting(*code);
    if (!after || setting->stepped) {
      values[*code] = from;
      continue;
    }
    const float linear = (time - before->time) / (after->time - before->time);
    const float share =
        animation.blend == PoseBlend::Linear ? linear : linear * linear * (3 - 2 * linear);
    values[*code] = from + (after->prop.at(*code) - from) * share;
  }
  return values;
}

// A turn in degrees (x, then y, then z, about the parent's axes) as a quaternion x, y, z, w, as
// the studio's editor makes it (animation.cjs quaternion).
inline std::array<float, 4> pose_quaternion(const std::array<float, 3>& turn) {
  auto about = [](int axis, float degrees) {
    const float half = degrees * 3.14159265358979f / 360.0f;
    std::array<float, 4> q{0, 0, 0, std::cos(half)};
    q[std::size_t(axis)] = std::sin(half);
    return q;
  };
  auto multiply = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return std::array<float, 4>{
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
  };
  return multiply(about(2, turn[2]), multiply(about(1, turn[1]), about(0, turn[0])));
}

}  // namespace kcd2s::integration
