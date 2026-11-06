#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/control.hpp"
#include "kcd2s/integration/guid.hpp"

// A script-driven animation of a player's body: the server's
// Player.playAnimation and Player.stopAnimation, and the playerAnimationEnd event. The animation
// is desired state the server keeps per player; other players see it through the owner's replicated
// animation, so only the owner's client gets the orders.
//
//   ANIMATION_ORDER : server -> the player's own client, on the control channel (0).
//                     Play: [tag][u8 1][varuint request][u8 flags][fragment][clip][tags]
//                           [varuint props, up to 2][prop ...]
//                     Stop: [tag][u8 2][varuint request][u8 flags]
//                     Texts are [varuint length][bytes], printable ASCII without spaces.
//                     Play flags: bit 0 loop, bit 1 lock the player's movement, bit 2 restore (the
//                     request already ended; put its props, tags and movement lock back without
//                     playing it again). A prop is [u8 flags: bit 0 left hand, bit 1 position,
//                     bit 2 rotation, bit 3 item, bit 4 in the hand only between the animation's
//                     own pickup and put-down][item: 16 GUID bytes, else the model:
//                     a text]
//                     [joint: a text, "" for the hand's own; it may hold spaces, as the game's
//                     joints do, but no '|'][position: 3 floats][rotation: 4
//                     floats, x y z w]. Stop flags: bit 0 stand up; its request is then the id of
//                     the stand-up (the game's StandUp or GetUp), else 0.
//                     Face: [tag][u8 3][varuint 0][u8 0][fragment][tags].
//                     Talk: [tag][u8 4][varuint 0][u8 0][style], "" for not talking.
//                     Play flag bit 6: the body keeps its heading while it runs;
//                     only the head and the camera turn (free look).
//                     Play flag bit 7: [varuint partner] follows the timing, the
//                     network id of the player the request is paired with (playPairedAnimation):
//                     the two bodies do not push each other while it runs. A [u8 role] follows
//                     it: 0 the master, 1 the slave, whose client places its body at the
//                     master's by the two clips' start locations.
//                     Play flag bit 5: [float speed][float start] follow the props,
//                     how fast it plays (0 holds it still, 1 as authored) and how many seconds
//                     into it it starts; without the bit 1 and 0.
//                     Speed: [tag][u8 5][varuint request][u8 0][float speed], the running
//                     request's new speed (Player.setAnimationSpeed, pause and resume).
//                     Props: [tag][u8 6][varuint request][u8 0][varuint props][prop ...], the
//                     running request's new props, without playing it again.
//                     A client before 0.3.4.2 refuses a Play with bit 5 and ignores Speed and
//                     Props.
//   ANIMATION_END   : the player's own client -> server, on the control channel (0), once per
//                     request: [tag][varuint request][u8 reason][u8 exit].
//                     The reason is how the request ended in the game's animation system: 1
//                     finished, 2 interrupted, 3 refused, 4 stopped, 5 replaced. The exit names
//                     the stand-up the client played for a stand-up request (0 none: the game
//                     authors no way out of the stance, nothing to report; 1 StandUp; 2 GetUp);
//                     0 for every other request.
namespace kcd2s::integration {

inline constexpr std::uint8_t kAnimationOrderTag = 55;
inline constexpr std::uint8_t kAnimationEndTag = 56;
// ANIMATION_PROP_CUE: the owner's client -> server, a prop of the running request
// that follows the animation went into the hand or was put down: [tag][request][prop u8][taken
// u8]. Peers before 0.3.4.1 ignore the tag.
inline constexpr std::uint8_t kAnimationPropCueTag = 161;
inline constexpr unsigned kMaxAnimationPropCuesPerSecond = 16;

// Bounds of the texts, longer than any fragment, tag list, joint or catalog path the game has.
inline constexpr std::size_t kAnimationFragmentMax = 64;
inline constexpr std::size_t kAnimationClipMax = 200;
inline constexpr std::size_t kAnimationTagsMax = 128;
inline constexpr std::size_t kAnimationJointMax = 64;
inline constexpr std::size_t kAnimationModelMax = 255;
inline constexpr std::size_t kAnimationPropsMax = 2;
// A prop's offset from its hand, in metres, at most this far on any axis.
inline constexpr float kAnimationOffsetMax = 100.0f;
// Orders a client holds before its game takes them (the oldest go first), and reports a client may
// send per second (one per request: a loop reports nothing until it is refused).
inline constexpr std::size_t kMaxAnimationOrders = 64;
inline constexpr unsigned kMaxAnimationEndsPerSecond = 32;
// A request id is a whole number a script can hold exactly (2^53 - 1).
inline constexpr std::uint64_t kAnimationRequestMax = (std::uint64_t(1) << 53) - 1;
// A request plays at most this many times as fast as authored, and starts at most this many
// seconds in.
inline constexpr float kAnimationSpeedMax = 10.0f;
inline constexpr float kAnimationStartMax = 600.0f;

struct AnimationProp {
  std::optional<Guid> item;  // an item class, else the model
  std::string model;         // a prop-catalog mesh path
  bool left_hand = false;
  std::string joint;
  std::optional<std::array<float, 3>> position;
  std::optional<std::array<float, 4>> rotation;  // x, y, z, w
  // In the hand only from the animation's own pickup (its pickup, spawnitem or attach event for
  // that hand) to its put-down (place, despawnitem, detach).
  bool from_animation = false;
  // In the hand from the start until the animation's own put-down (place, despawnitem, detach)
  // for that hand; never with from_animation.
  bool until_put_down = false;
  // A prop following the animation may rest in the world before and after: the
  // server Prop of this id is hidden while the animation holds the item, 0 for none.
  std::uint32_t rest = 0;

  friend bool operator==(const AnimationProp& a, const AnimationProp& b) {
    return a.item == b.item && a.model == b.model && a.left_hand == b.left_hand &&
           a.joint == b.joint && a.position == b.position && a.rotation == b.rotation &&
           a.from_animation == b.from_animation && a.until_put_down == b.until_put_down &&
           a.rest == b.rest;
  }
};

struct AnimationOrder {
  enum class Kind : std::uint8_t {
    Play = 1,
    Stop = 2,
    // Player.setExpression: `fragment` (empty hands the face back) with `tags`
    // on the face's own scope, beside the body's request; no request id.
    Face = 3,
    // Player.setSpeaking: `fragment` is the style the mouth moves in (neutral, happy,
    // drunk, chew), empty when it does not; no request id.
    Talk = 4,
    // Player.setAnimationSpeed, pauseAnimation and resumeAnimation: `speed` of the
    // running request `request`.
    Speed = 5,
    // Player.setAnimationProps: `props` of the running request `request`.
    Props = 6,
  };
  Kind kind = Kind::Play;
  // The request this order starts; for Stop the stand-up request, 0 without a stand-up.
  std::uint64_t request = 0;
  std::string fragment;
  std::string clip;
  std::string tags;
  bool loop = false;
  bool lock_movement = false;
  bool restore = false;
  // Only the upper body plays it: the fragment on the Mannequin UpperBody scope, the
  // legs keep the walk, and moving does not end it.
  bool upper_body = false;
  // The body has no gravity while it runs: a script lifts it into a noose.
  bool no_gravity = false;
  // The body keeps its heading while it runs; only the head and the camera turn.
  bool lock_rotation = false;
  // How far the view turns then (playAnimation lockRotation: { yaw, up, down }), in degrees; 0
  // yaw keeps the defaults (120 either side, 70 up and down). With a yaw the view begins where the
  // body faces, at the middle of the up and down range (a seat at a board looks down at it).
  std::uint8_t look_yaw = 0;
  std::uint8_t look_up = 0;
  std::uint8_t look_down = 0;
  // The player it is paired with (playPairedAnimation), 0 for none, and whether this
  // body is the pair's slave: its client places it at the master's by the clips' start locations.
  std::uint32_t partner = 0;
  bool paired_slave = false;
  // How fast it plays (0 holds it still) and how many seconds into it it starts.
  float speed = 1.0f;
  float start = 0.0f;
  std::vector<AnimationProp> props;
  bool stand_up = false;

  friend bool operator==(const AnimationOrder& a, const AnimationOrder& b) {
    return a.kind == b.kind && a.request == b.request && a.fragment == b.fragment &&
           a.clip == b.clip && a.tags == b.tags && a.loop == b.loop &&
           a.lock_movement == b.lock_movement && a.restore == b.restore &&
           a.upper_body == b.upper_body && a.no_gravity == b.no_gravity &&
           a.lock_rotation == b.lock_rotation && a.look_yaw == b.look_yaw &&
           a.look_up == b.look_up && a.look_down == b.look_down && a.partner == b.partner &&
           a.paired_slave == b.paired_slave && a.speed == b.speed && a.start == b.start &&
           a.props == b.props && a.stand_up == b.stand_up;
  }
};

enum class AnimationReason : std::uint8_t {
  Finished = 1,
  Interrupted = 2,
  Refused = 3,
  Stopped = 4,
  Replaced = 5,
};

enum class AnimationExit : std::uint8_t {
  None = 0,
  StandUp = 1,
  GetUp = 2,
};

struct AnimationEnd {
  std::uint64_t request = 0;
  AnimationReason reason = AnimationReason::Finished;
  AnimationExit exit = AnimationExit::None;

  friend bool operator==(const AnimationEnd& a, const AnimationEnd& b) {
    return a.request == b.request && a.reason == b.reason && a.exit == b.exit;
  }
};

namespace animation_detail {

// Printable ASCII without spaces, at most `max` bytes.
inline bool text_valid(const std::string& text, std::size_t max) {
  std::size_t printable = 0;
  while (printable < text.size() && text[printable] >= 0x21 && text[printable] <= 0x7E) {
    ++printable;
  }
  return printable == text.size() && text.size() <= max;
}

// A joint's name: the game's have spaces ("Bip01 Neck"), so printable ASCII with the
// space, but not kJointSpace, which the props field writes for a space.
inline constexpr char kJointSpace = '|';
inline bool joint_valid(const std::string& text) {
  if (text.size() > kAnimationJointMax) {
    return false;
  }
  for (const char c : text) {
    if (c < 0x20 || c > 0x7E || c == kJointSpace) {
      return false;
    }
  }
  return true;
}

inline bool offset_valid(float value) {
  return std::isfinite(value) && std::fabs(value) <= kAnimationOffsetMax;
}

inline bool speed_valid(float speed) {
  return std::isfinite(speed) && speed >= 0.0f && speed <= kAnimationSpeedMax;
}

inline bool start_valid(float start) {
  return std::isfinite(start) && start >= 0.0f && start <= kAnimationStartMax;
}

inline bool prop_valid(const AnimationProp& prop) {
  if (prop.item.has_value() != prop.model.empty() || !text_valid(prop.model, kAnimationModelMax) ||
      !joint_valid(prop.joint)) {
    return false;
  }
  if (prop.position) {
    for (const float value : *prop.position) {
      if (!offset_valid(value)) {
        return false;
      }
    }
  }
  if (prop.rotation) {
    double length_squared = 0;
    for (const float value : *prop.rotation) {
      if (!std::isfinite(value)) {
        return false;
      }
      length_squared += double(value) * double(value);
    }
    if (std::fabs(length_squared - 1.0) > 0.01) {
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

// A text's bytes as they came, up to max; false when they do not fit.
inline bool read_bytes(core::BitReader& r, std::string& text, std::size_t max) {
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

inline bool read_text(core::BitReader& r, std::string& text, std::size_t max) {
  return read_bytes(r, text, max) && text_valid(text, max);
}

inline void write_floats(core::BitWriter& w, const float* values, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    detail::write_control_f32(w, values[i]);
  }
}

inline void read_floats(core::BitReader& r, float* values, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    values[i] = detail::read_control_f32(r);
  }
}

constexpr std::uint8_t kLoopBit = 1;
constexpr std::uint8_t kLockBit = 2;
constexpr std::uint8_t kRestoreBit = 4;
constexpr std::uint8_t kUpperBodyBit = 8;
constexpr std::uint8_t kNoGravityBit = 16;
constexpr std::uint8_t kTimingBit = 32;
constexpr std::uint8_t kLockRotationBit = 64;
constexpr std::uint8_t kPartnerBit = 128;
constexpr std::uint8_t kLeftHandBit = 1;
constexpr std::uint8_t kPositionBit = 2;
constexpr std::uint8_t kRotationBit = 4;
constexpr std::uint8_t kItemBit = 8;
constexpr std::uint8_t kFromAnimationBit = 16;
constexpr std::uint8_t kRestBit = 32;
constexpr std::uint8_t kUntilPutDownBit = 64;

inline void write_props(core::BitWriter& w, const std::vector<AnimationProp>& props) {
  w.write_varuint(props.size());
  for (const AnimationProp& prop : props) {
    w.write_bits(
        std::uint8_t((prop.left_hand ? kLeftHandBit : 0) | (prop.position ? kPositionBit : 0) |
                     (prop.rotation ? kRotationBit : 0) | (prop.item ? kItemBit : 0) |
                     (prop.from_animation ? kFromAnimationBit : 0) |
                     (prop.rest != 0 ? kRestBit : 0) |
                     (prop.until_put_down ? kUntilPutDownBit : 0)),
        8);
    if (prop.item) {
      for (const std::uint8_t byte : *prop.item) {
        w.write_bits(byte, 8);
      }
    } else {
      write_text(w, prop.model);
    }
    write_text(w, prop.joint);
    if (prop.position) {
      write_floats(w, prop.position->data(), 3);
    }
    if (prop.rotation) {
      write_floats(w, prop.rotation->data(), 4);
    }
    if (prop.rest != 0) {
      w.write_varuint(prop.rest);
    }
  }
}

inline bool read_props(core::BitReader& r, std::vector<AnimationProp>& props) {
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || count > kAnimationPropsMax) {
    return false;
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    AnimationProp prop;
    const std::uint64_t prop_flags = r.read_bits(8);
    if (r.overrun() ||
        (prop_flags & ~std::uint64_t(kLeftHandBit | kPositionBit | kRotationBit | kItemBit |
                                     kFromAnimationBit | kRestBit | kUntilPutDownBit)) != 0) {
      return false;
    }
    prop.left_hand = (prop_flags & kLeftHandBit) != 0;
    prop.from_animation = (prop_flags & kFromAnimationBit) != 0;
    prop.until_put_down = (prop_flags & kUntilPutDownBit) != 0;
    if (prop.from_animation && prop.until_put_down) {
      return false;
    }
    if ((prop_flags & kItemBit) != 0) {
      Guid item{};
      for (std::uint8_t& byte : item) {
        byte = std::uint8_t(r.read_bits(8));
      }
      prop.item = item;
    } else if (!read_text(r, prop.model, kAnimationModelMax)) {
      return false;
    }
    if (!read_bytes(r, prop.joint, kAnimationJointMax) || !joint_valid(prop.joint)) {
      return false;
    }
    if ((prop_flags & kPositionBit) != 0) {
      std::array<float, 3> position{};
      read_floats(r, position.data(), 3);
      prop.position = position;
    }
    if ((prop_flags & kRotationBit) != 0) {
      std::array<float, 4> rotation{};
      read_floats(r, rotation.data(), 4);
      prop.rotation = rotation;
    }
    if ((prop_flags & kRestBit) != 0) {
      const std::uint64_t rest = r.read_varuint();
      if (r.overrun() || rest == 0 || rest > 0xFFFFFFFFu || !prop.from_animation) {
        return false;
      }
      prop.rest = std::uint32_t(rest);
    }
    props.push_back(std::move(prop));
  }
  return true;
}

}  // namespace animation_detail

// A talking style the client knows, or "" for not talking.
inline bool talk_style_valid(const std::string& style) {
  return style.empty() || style == "neutral" || style == "happy" || style == "drunk" ||
         style == "chew";
}

// Whether the server may send `order`: a Play names a fragment, a clip (with no fragment), tags or
// props; a Stop carries a stand-up request exactly when it asks for the stand-up.
inline bool animation_order_valid(const AnimationOrder& order) {
  using namespace animation_detail;
  if (order.request > kAnimationRequestMax) {
    return false;
  }
  if (order.look_yaw > 180 || order.look_up > 89 || order.look_down > 89) {
    return false;
  }
  if (order.kind == AnimationOrder::Kind::Talk) {
    return order.request == 0 && talk_style_valid(order.fragment) && order.tags.empty() &&
           order.clip.empty() && !order.loop && !order.lock_movement && !order.restore &&
           order.props.empty() && !order.stand_up;
  }
  if (order.kind == AnimationOrder::Kind::Face) {
    return order.request == 0 && text_valid(order.fragment, kAnimationFragmentMax) &&
           text_valid(order.tags, kAnimationTagsMax) && order.clip.empty() && !order.loop &&
           !order.lock_movement && !order.restore && order.props.empty() && !order.stand_up;
  }
  if (order.kind == AnimationOrder::Kind::Stop) {
    return (order.request != 0) == order.stand_up && order.fragment.empty() && order.clip.empty() &&
           order.tags.empty() && !order.loop && !order.lock_movement && !order.restore &&
           order.props.empty();
  }
  const bool only_request = order.request != 0 && order.fragment.empty() && order.clip.empty() &&
                            order.tags.empty() && !order.loop && !order.lock_movement &&
                            !order.restore && !order.upper_body && !order.no_gravity &&
                            !order.lock_rotation && order.partner == 0 && !order.paired_slave &&
                            !order.stand_up && order.start == 0.0f;
  if (order.kind == AnimationOrder::Kind::Speed) {
    return only_request && speed_valid(order.speed) && order.props.empty();
  }
  if (order.kind == AnimationOrder::Kind::Props) {
    if (!only_request || order.speed != 1.0f || order.props.size() > kAnimationPropsMax) {
      return false;
    }
    for (const AnimationProp& prop : order.props) {
      if (!prop_valid(prop)) {
        return false;
      }
    }
    return true;
  }
  if (!speed_valid(order.speed) || !start_valid(order.start)) {
    return false;
  }
  if (order.kind != AnimationOrder::Kind::Play || order.request == 0 || order.stand_up ||
      !text_valid(order.fragment, kAnimationFragmentMax) ||
      !text_valid(order.clip, kAnimationClipMax) || !text_valid(order.tags, kAnimationTagsMax) ||
      (!order.fragment.empty() && !order.clip.empty()) || order.props.size() > kAnimationPropsMax ||
      (order.paired_slave && order.partner == 0)) {
    return false;
  }
  if (order.fragment.empty() && order.clip.empty() && order.tags.empty() && order.props.empty()) {
    return false;
  }
  for (const AnimationProp& prop : order.props) {
    if (!prop_valid(prop)) {
      return false;
    }
  }
  return true;
}

// Empty when `order` may not be sent.
inline std::vector<std::uint8_t> encode_animation_order(const AnimationOrder& order) {
  using namespace animation_detail;
  if (!animation_order_valid(order)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kAnimationOrderTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  w.write_varuint(order.request);
  if (order.kind == AnimationOrder::Kind::Stop) {
    w.write_bits(order.stand_up ? 1 : 0, 8);
    return w.bytes();
  }
  if (order.kind == AnimationOrder::Kind::Face) {
    w.write_bits(0, 8);
    write_text(w, order.fragment);
    write_text(w, order.tags);
    return w.bytes();
  }
  if (order.kind == AnimationOrder::Kind::Talk) {
    w.write_bits(0, 8);
    write_text(w, order.fragment);
    return w.bytes();
  }
  if (order.kind == AnimationOrder::Kind::Speed) {
    w.write_bits(0, 8);
    write_floats(w, &order.speed, 1);
    return w.bytes();
  }
  if (order.kind == AnimationOrder::Kind::Props) {
    w.write_bits(0, 8);
    write_props(w, order.props);
    return w.bytes();
  }
  const bool timed = order.speed != 1.0f || order.start != 0.0f;
  w.write_bits(
      std::uint8_t((order.loop ? kLoopBit : 0) | (order.lock_movement ? kLockBit : 0) |
                   (order.restore ? kRestoreBit : 0) | (order.upper_body ? kUpperBodyBit : 0) |
                   (order.no_gravity ? kNoGravityBit : 0) | (timed ? kTimingBit : 0) |
                   (order.lock_rotation ? kLockRotationBit : 0) |
                   (order.partner != 0 ? kPartnerBit : 0)),
      8);
  write_text(w, order.fragment);
  write_text(w, order.clip);
  write_text(w, order.tags);
  write_props(w, order.props);
  if (timed) {
    write_floats(w, &order.speed, 1);
    write_floats(w, &order.start, 1);
  }
  if (order.partner != 0) {
    w.write_varuint(order.partner);
    w.write_bits(order.paired_slave ? 1 : 0, 8);
  }
  if (order.lock_rotation) {
    // The view's limits: yaw 0 .. 180, up and down 0 .. 89 (0: the default).
    w.write_bits(order.look_yaw, 8);
    w.write_bits(order.look_up, 8);
    w.write_bits(order.look_down, 8);
  }
  return w.bytes();
}

inline bool decode_animation_order(const std::vector<std::uint8_t>& p, AnimationOrder& order) {
  using namespace animation_detail;
  if (p.empty() || p[0] != kAnimationOrderTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  AnimationOrder decoded;
  const std::uint64_t kind = r.read_bits(8);
  decoded.request = r.read_varuint();
  const std::uint64_t flags = r.read_bits(8);
  if (r.overrun()) {
    return false;
  }
  if (kind == std::uint64_t(AnimationOrder::Kind::Talk)) {
    decoded.kind = AnimationOrder::Kind::Talk;
    if (flags != 0 || decoded.request != 0 ||
        !read_text(r, decoded.fragment, kAnimationFragmentMax) ||
        !talk_style_valid(decoded.fragment)) {
      return false;
    }
  } else if (kind == std::uint64_t(AnimationOrder::Kind::Face)) {
    decoded.kind = AnimationOrder::Kind::Face;
    if (flags != 0 || decoded.request != 0 ||
        !read_text(r, decoded.fragment, kAnimationFragmentMax) ||
        !read_text(r, decoded.tags, kAnimationTagsMax)) {
      return false;
    }
  } else if (kind == std::uint64_t(AnimationOrder::Kind::Speed)) {
    decoded.kind = AnimationOrder::Kind::Speed;
    if (flags != 0) {
      return false;
    }
    read_floats(r, &decoded.speed, 1);
  } else if (kind == std::uint64_t(AnimationOrder::Kind::Props)) {
    decoded.kind = AnimationOrder::Kind::Props;
    if (flags != 0 || !read_props(r, decoded.props)) {
      return false;
    }
  } else if (kind == std::uint64_t(AnimationOrder::Kind::Stop)) {
    decoded.kind = AnimationOrder::Kind::Stop;
    if (flags > 1) {
      return false;
    }
    decoded.stand_up = flags == 1;
  } else if (kind == std::uint64_t(AnimationOrder::Kind::Play)) {
    if ((flags & ~std::uint64_t(kLoopBit | kLockBit | kRestoreBit | kUpperBodyBit | kNoGravityBit |
                                kTimingBit | kLockRotationBit | kPartnerBit)) != 0) {
      return false;
    }
    decoded.kind = AnimationOrder::Kind::Play;
    decoded.loop = (flags & kLoopBit) != 0;
    decoded.lock_movement = (flags & kLockBit) != 0;
    decoded.restore = (flags & kRestoreBit) != 0;
    decoded.upper_body = (flags & kUpperBodyBit) != 0;
    decoded.no_gravity = (flags & kNoGravityBit) != 0;
    decoded.lock_rotation = (flags & kLockRotationBit) != 0;
    if (!read_text(r, decoded.fragment, kAnimationFragmentMax) ||
        !read_text(r, decoded.clip, kAnimationClipMax) ||
        !read_text(r, decoded.tags, kAnimationTagsMax)) {
      return false;
    }
    if (!read_props(r, decoded.props)) {
      return false;
    }
    if ((flags & kTimingBit) != 0) {
      read_floats(r, &decoded.speed, 1);
      read_floats(r, &decoded.start, 1);
    }
    if ((flags & kPartnerBit) != 0) {
      const std::uint64_t partner = r.read_varuint();
      if (r.overrun() || partner == 0 || partner > 0xFFFFFFFFu) {
        return false;
      }
      decoded.partner = std::uint32_t(partner);
      const std::uint64_t role = r.read_bits(8);
      if (r.overrun() || role > 1) {
        return false;
      }
      decoded.paired_slave = role == 1;
    }
    if (decoded.lock_rotation) {
      const std::uint64_t yaw = r.read_bits(8);
      const std::uint64_t up = r.read_bits(8);
      const std::uint64_t down = r.read_bits(8);
      if (r.overrun() || yaw > 180 || up > 89 || down > 89) {
        return false;
      }
      decoded.look_yaw = std::uint8_t(yaw);
      decoded.look_up = std::uint8_t(up);
      decoded.look_down = std::uint8_t(down);
    }
  } else {
    return false;
  }
  if (r.overrun() || r.bits_remaining() != 0 || !animation_order_valid(decoded)) {
    return false;
  }
  order = std::move(decoded);
  return true;
}

// Empty for a request id that is 0 or too large, an unknown reason or an unknown exit.
inline std::vector<std::uint8_t> encode_animation_end(const AnimationEnd& end) {
  const auto reason = std::uint8_t(end.reason);
  const auto exit = std::uint8_t(end.exit);
  if (end.request == 0 || end.request > kAnimationRequestMax || reason < 1 || reason > 5 ||
      exit > 2) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kAnimationEndTag, 8);
  w.write_varuint(end.request);
  w.write_bits(reason, 8);
  w.write_bits(exit, 8);
  return w.bytes();
}

struct AnimationPropCue {
  std::uint64_t request = 0;
  std::uint8_t prop = 0;  // its index in the request's props
  bool taken = false;     // into the hand, else put down
};

// Empty for a request id that is 0 or too large or a prop index past kAnimationPropsMax.
inline std::vector<std::uint8_t> encode_animation_prop_cue(const AnimationPropCue& cue) {
  if (cue.request == 0 || cue.request > kAnimationRequestMax || cue.prop >= kAnimationPropsMax) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kAnimationPropCueTag, 8);
  w.write_varuint(cue.request);
  w.write_bits(cue.prop, 8);
  w.write_bits(cue.taken ? 1u : 0u, 8);
  return w.bytes();
}

inline bool decode_animation_prop_cue(const std::vector<std::uint8_t>& p, AnimationPropCue& cue) {
  if (p.empty() || p[0] != kAnimationPropCueTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t request = r.read_varuint();
  const std::uint64_t prop = r.read_bits(8);
  const std::uint64_t taken = r.read_bits(8);
  if (r.overrun() || r.bits_remaining() != 0 || request == 0 || request > kAnimationRequestMax ||
      prop >= kAnimationPropsMax || taken > 1) {
    return false;
  }
  cue.request = request;
  cue.prop = std::uint8_t(prop);
  cue.taken = taken == 1;
  return true;
}

inline bool decode_animation_end(const std::vector<std::uint8_t>& p, AnimationEnd& end) {
  if (p.empty() || p[0] != kAnimationEndTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t request = r.read_varuint();
  const std::uint64_t reason = r.read_bits(8);
  const std::uint64_t exit = r.read_bits(8);
  if (r.overrun() || r.bits_remaining() != 0 || request == 0 || request > kAnimationRequestMax ||
      reason < 1 || reason > 5 || exit > 2) {
    return false;
  }
  end.request = request;
  end.reason = AnimationReason(reason);
  end.exit = AnimationExit(exit);
  return true;
}

}  // namespace kcd2s::integration
