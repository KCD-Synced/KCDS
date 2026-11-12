#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "kcd2s/core/float_chars.hpp"
#include "kcd2s/integration/guid.hpp"
#include "kcd2s/integration/player_animation.hpp"

// The props a player's script animation puts in the player's hands
// (Player.playAnimation's `props`), as every client shows them. The owner's client publishes the
// props of the request its body holds in the NetworkPlayer field "animationProps"
// (animation_props_field), from the request's start until it is stopped or replaced (also after its
// fragment ended); every client, the owner's own too, shows the field's props on that player's
// body (Client/runtime/animation/animation_props.hpp). The field changes only with a request, so it
// costs nothing between them.
//
// The field: the props one after another, "" for none. A prop is words separated by one space: its
// hand ("R" or "L"); what it is ("I" and the item class's GUID in guid_text's form, or "M" and the
// mesh's path); "J" and the joint ("J" alone for the hand's own); and only when the request gave
// them, "P" and the offset "x,y,z" in metres and "Q" and the rotation "x,y,z,w"; and "A" for a
// prop in the hand only between the animation's own pickup and put-down. The texts are
// printable ASCII without spaces (the order's own bounds), so a space never falls inside a word; a
// joint may have spaces (the game's "Bip01 Neck"), written there as '|'.
//
// A clip a resource ships plays on every client from the same field (animation_field): a clip's
// id in a body's animation set is that client's own, so it cannot go by id like the game's clips.
// While the owner's body plays one, the field starts with the word "C", "1" for a loop or "0" for
// once, and the clip ("<resource>/<path>", ShippedClip); the props follow it.
//
// While the owner's mouth moves (Player.setSpeaking), the field starts with the word
// "T" and the style (talking_field). The face clip a style loops has an id past what the clip sync
// carries, so every client loops it by name on that player's body from here.
//
// While the owner's request plays at a speed of its own or from a start time, the
// field starts with the word "S", the speed it plays at now, the layer it plays on and how far
// through its clip that layer is ("S0.50:0:0.42", speed_field): every other client plays the
// replica's layers at that speed and moves its clip there when it is off. The place is in steps
// of 0.02, so the field changes a few times a second while such a request plays, and not while it
// is held still.
//
// While the owner points, the word "P" and the point the arm points at come next
// ("P1430.5,2580.2,64.1", pointing_field): every other client aims the replica's arm there.
namespace kcd2s::integration {

// A clip a resource ships, as the server names it in an order's clip ("@<resource>/<path>"): the
// resource's name and the file's path in it.
struct ShippedClip {
  std::string clip;  // "<resource>/<path>"
  bool loop = false;
  bool operator==(const ShippedClip&) const = default;
};

// The order's form of a shipped clip starts with this.
inline constexpr char kShippedClipMark = '@';

// Whether text is a shipped clip without its mark: a resource name and a path below it, printable
// ASCII without spaces, no "." or ".." part, within the order's clip bound.
inline bool shipped_clip_valid(std::string_view clip) {
  if (clip.empty() || clip.size() + 1 > kAnimationClipMax ||
      !animation_detail::text_valid(std::string(clip), kAnimationClipMax)) {
    return false;
  }
  const std::size_t slash = clip.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 == clip.size()) {
    return false;
  }
  std::size_t begin = 0;
  while (begin <= clip.size()) {
    std::size_t end = clip.find('/', begin);
    if (end == std::string_view::npos) {
      end = clip.size();
    }
    const std::string_view part = clip.substr(begin, end - begin);
    if (part.empty() || part == "." || part == "..") {
      return false;
    }
    begin = end + 1;
  }
  return clip.find('\\') == std::string_view::npos && clip.find(':') == std::string_view::npos;
}

// The shipped clip an order's clip names ("@<resource>/<path>"); false for a clip of the game's.
inline bool shipped_clip_of(const std::string& order_clip, ShippedClip& shipped) {
  if (order_clip.empty() || order_clip[0] != kShippedClipMark) {
    return false;
  }
  const std::string_view clip = std::string_view(order_clip).substr(1);
  if (!shipped_clip_valid(clip)) {
    return false;
  }
  shipped.clip = std::string(clip);
  return true;
}

namespace animation_props_detail {

inline void append_floats(std::string& out, const float* values, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    char text[48];
    const auto written =
        std::to_chars(text, text + sizeof(text), values[i], std::chars_format::fixed, 4);
    out.append(text, written.ptr);
  }
}

// Exactly `count` numbers separated by commas, the whole of text.
inline bool parse_floats(std::string_view text, float* values, std::size_t count) {
  const char* at = text.data();
  const char* const end = text.data() + text.size();
  for (std::size_t i = 0; i < count; ++i) {
    if (i > 0) {
      if (at == end || *at != ',') {
        return false;
      }
      ++at;
    }
    const auto read = core::float_from_chars(at, end, values[i]);
    if (read.ec != std::errc() || read.ptr == at) {
      return false;
    }
    at = read.ptr;
  }
  return at == end;
}

// The words of text, split at single spaces; false for an empty word (two spaces, or a space at
// either end).
inline bool split_words(std::string_view text, std::vector<std::string_view>& words) {
  std::size_t begin = 0;
  while (begin <= text.size()) {
    std::size_t end = text.find(' ', begin);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    if (end == begin) {
      return false;
    }
    words.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  return true;
}

}  // namespace animation_props_detail

// The field of a request's props; "" for none, and for props an order could not carry.
inline std::string animation_props_field(const std::vector<AnimationProp>& props) {
  if (props.size() > kAnimationPropsMax) {
    return {};
  }
  std::string field;
  for (const auto& prop : props) {
    if (!animation_detail::prop_valid(prop)) {
      return {};
    }
    if (!field.empty()) {
      field.push_back(' ');
    }
    field += prop.left_hand ? "L" : "R";
    if (prop.item) {
      field += " I" + guid_text(*prop.item);
    } else {
      field += " M" + prop.model;
    }
    std::string joint = prop.joint;
    std::replace(joint.begin(), joint.end(), ' ', animation_detail::kJointSpace);
    field += " J" + joint;
    if (prop.position) {
      field += " P";
      animation_props_detail::append_floats(field, prop.position->data(), 3);
    }
    if (prop.rotation) {
      field += " Q";
      animation_props_detail::append_floats(field, prop.rotation->data(), 4);
    }
    if (prop.from_animation) {
      field += " A";
    }
    if (prop.until_put_down) {
      field += " D";
    }
    if (prop.rest != 0) {
      field += " S" + std::to_string(prop.rest);
    }
  }
  return field;
}

// The props of a field; false (and none) for anything animation_props_field does not write.
inline bool parse_animation_props_field(const std::string& field,
                                        std::vector<AnimationProp>& props) {
  props.clear();
  if (field.empty()) {
    return true;
  }
  std::vector<std::string_view> words;
  if (!animation_props_detail::split_words(field, words)) {
    return false;
  }
  std::vector<AnimationProp> parsed;
  std::size_t next = 0;
  while (next < words.size()) {
    if (parsed.size() == kAnimationPropsMax || next + 3 > words.size()) {
      return false;
    }
    AnimationProp prop;
    const std::string_view hand = words[next];
    const std::string_view what = words[next + 1];
    const std::string_view joint = words[next + 2];
    next += 3;
    if (hand != "R" && hand != "L") {
      return false;
    }
    prop.left_hand = hand == "L";
    if (what.size() > 1 && what[0] == 'I') {
      Guid item{};
      if (!parse_guid(std::string(what.substr(1)), item)) {
        return false;
      }
      prop.item = item;
    } else if (what.size() > 1 && what[0] == 'M') {
      prop.model = std::string(what.substr(1));
    } else {
      return false;
    }
    if (joint.empty() || joint[0] != 'J') {
      return false;
    }
    prop.joint = std::string(joint.substr(1));
    std::replace(prop.joint.begin(), prop.joint.end(), animation_detail::kJointSpace, ' ');
    if (next < words.size() && words[next][0] == 'P') {
      std::array<float, 3> position{};
      if (!animation_props_detail::parse_floats(words[next].substr(1), position.data(), 3)) {
        return false;
      }
      prop.position = position;
      ++next;
    }
    if (next < words.size() && words[next][0] == 'Q') {
      std::array<float, 4> rotation{};
      if (!animation_props_detail::parse_floats(words[next].substr(1), rotation.data(), 4)) {
        return false;
      }
      prop.rotation = rotation;
      ++next;
    }
    if (next < words.size() && words[next] == "A") {
      prop.from_animation = true;
      ++next;
    }
    // Held from the start until the animation puts it down.
    if (next < words.size() && words[next] == "D") {
      if (prop.from_animation) {
        return false;
      }
      prop.until_put_down = true;
      ++next;
    }
    // The server Prop it rests as before and after.
    if (next < words.size() && words[next].size() > 1 && words[next][0] == 'S') {
      std::uint64_t rest = 0;
      for (const char c : words[next].substr(1)) {
        if (c < '0' || c > '9' || rest > 0xFFFFFFFFu / 10) {
          return false;
        }
        rest = rest * 10 + std::uint64_t(c - '0');
      }
      if (rest == 0 || rest > 0xFFFFFFFFu || !prop.from_animation) {
        return false;
      }
      prop.rest = std::uint32_t(rest);
      ++next;
    }
    if (!animation_detail::prop_valid(prop)) {
      return false;
    }
    parsed.push_back(std::move(prop));
  }
  props = std::move(parsed);
  return true;
}

// The whole field: the shipped clip the owner's body plays (none: no clip word) and the props;
// "" for nothing, and for a clip or props an order could not carry.
inline std::string animation_field(const std::optional<ShippedClip>& clip,
                                   const std::vector<AnimationProp>& props) {
  const std::string props_field = animation_props_field(props);
  if (!props.empty() && props_field.empty()) {
    return {};
  }
  if (!clip) {
    return props_field;
  }
  if (!shipped_clip_valid(clip->clip)) {
    return {};
  }
  std::string field = std::string("C") + (clip->loop ? "1" : "0") + clip->clip;
  if (!props_field.empty()) {
    field += " " + props_field;
  }
  return field;
}

// The field with the owner's speed mark ("<speed>:<layer>:<place>", without the "S") in front,
// the field alone for no mark.
inline std::string speed_field(const std::string& mark, const std::string& field) {
  if (mark.empty()) {
    return field;
  }
  return field.empty() ? "S" + mark : "S" + mark + " " + field;
}

// Splits the speed mark off a field: the mark ("" without one, without its "S") and the rest.
inline void split_speed_field(const std::string& field, std::string& mark, std::string& rest) {
  mark.clear();
  rest = field;
  if (field.size() < 2 || field[0] != 'S' || field[1] < '0' || field[1] > '9') {
    return;
  }
  const std::size_t space = field.find(' ');
  mark = field.substr(1, space == std::string::npos ? std::string::npos : space - 1);
  rest = space == std::string::npos ? std::string() : field.substr(space + 1);
}

// The speed (0 to 20: a start time is reached at up to 20 times), layer (0 to 15) and place (0 to
// 1) of a speed mark; false for a mark speed_field was not given.
inline bool parse_speed_mark(const std::string& mark, float& speed, unsigned& layer, float& place) {
  const std::size_t first = mark.find(':');
  const std::size_t second = first == std::string::npos ? first : mark.find(':', first + 1);
  if (second == std::string::npos) {
    return false;
  }
  const char* text = mark.data();
  const auto speed_read = core::float_from_chars(text, text + first, speed);
  const auto layer_read = std::from_chars(text + first + 1, text + second, layer);
  const auto place_read = core::float_from_chars(text + second + 1, text + mark.size(), place);
  return speed_read.ec == std::errc() && speed_read.ptr == text + first &&
         layer_read.ec == std::errc() && layer_read.ptr == text + second &&
         place_read.ec == std::errc() && place_read.ptr == text + mark.size() && speed >= 0.0f &&
         speed <= 20.0f && layer < 16 && place >= 0.0f && place <= 1.0f;
}

// The field with the owner's pointing mark "P" and the point the arm points at ("x,y,z" in metres,
// the mark given without its "P") in front, while the owner points; the field alone
// for no mark.
inline std::string pointing_field(const std::string& mark, const std::string& field) {
  if (mark.empty()) {
    return field;
  }
  return field.empty() ? "P" + mark : "P" + mark + " " + field;
}

// Splits the pointing mark off a field: the mark ("" without one, without its "P") and the rest.
inline void split_pointing_field(const std::string& field, std::string& mark, std::string& rest) {
  mark.clear();
  rest = field;
  if (field.size() < 2 || field[0] != 'P' ||
      !((field[1] >= '0' && field[1] <= '9') || field[1] == '-')) {
    return;
  }
  const std::size_t space = field.find(' ');
  mark = field.substr(1, space == std::string::npos ? std::string::npos : space - 1);
  rest = space == std::string::npos ? std::string() : field.substr(space + 1);
}

// The point of a pointing mark; false for a mark pointing_field was not given (or a point out of
// any level).
inline bool parse_pointing_mark(const std::string& mark, float point[3]) {
  const char* at = mark.data();
  const char* end = mark.data() + mark.size();
  for (int axis = 0; axis < 3; ++axis) {
    const auto read = core::float_from_chars(at, end, point[axis]);
    if (read.ec != std::errc() || !(point[axis] > -1.0e6f && point[axis] < 1.0e6f)) {
      return false;
    }
    at = read.ptr;
    if (axis < 2) {
      if (at == end || *at != ',') {
        return false;
      }
      ++at;
    }
  }
  return at == end;
}

// The field with the upper-body mark "U" in front while the owner's request moves only the upper
// body; the field alone otherwise.
inline std::string upper_body_field(bool upper_body, const std::string& field) {
  if (!upper_body) {
    return field;
  }
  return field.empty() ? std::string("U") : "U " + field;
}

// Splits the upper-body mark off a field; `rest` is the field without it.
inline void split_upper_body_field(const std::string& field, bool& upper_body, std::string& rest) {
  upper_body = field == "U" || field.rfind("U ", 0) == 0;
  rest = !upper_body ? field : field.size() > 2 ? field.substr(2) : std::string();
}

// The field with the talking style in front of it; the field alone while not talking ("").
inline std::string talking_field(const std::string& style, const std::string& field) {
  if (style.empty() || !talk_style_valid(style)) {
    return field;
  }
  return field.empty() ? "T" + style : "T" + style + " " + field;
}

// Takes the talking word off a field: its style ("" without one) and the rest; false for a
// talking word talking_field does not write.
inline bool split_talking_field(const std::string& field, std::string& style, std::string& rest) {
  style.clear();
  rest = field;
  if (field.empty() || field[0] != 'T') {
    return true;
  }
  const std::size_t space = field.find(' ');
  const std::string word = field.substr(0, space);
  if (word.size() < 2 || !talk_style_valid(word.substr(1))) {
    return false;
  }
  if (space != std::string::npos && space + 1 == field.size()) {
    return false;
  }
  style = word.substr(1);
  rest = space == std::string::npos ? std::string() : field.substr(space + 1);
  return true;
}

// Splits a field into its shipped clip (none without a clip word) and its props' part, which
// parse_animation_props_field reads; false for a clip word animation_field does not write.
inline bool split_animation_field(const std::string& field, std::optional<ShippedClip>& clip,
                                  std::string& props_field) {
  clip.reset();
  props_field.clear();
  if (field.empty() || field[0] != 'C') {
    props_field = field;
    return true;
  }
  const std::size_t space = field.find(' ');
  const std::string word = field.substr(0, space);
  if (word.size() < 3 || (word[1] != '0' && word[1] != '1') ||
      !shipped_clip_valid(std::string_view(word).substr(2))) {
    return false;
  }
  if (space != std::string::npos) {
    if (space + 1 == field.size()) {
      return false;
    }
    props_field = field.substr(space + 1);
  }
  clip = ShippedClip{word.substr(2), word[1] == '1'};
  return true;
}

}  // namespace kcd2s::integration
