#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>

// A player's physical prop (Asset/Animation Studio): a rope or a whip
// in a hand, simulated by the game's own rope physics on every client. The server sets it
// (Player.setPhysicalProp); the NetworkPlayer field `physicalProp` carries it to every client, a
// player who joins later too, as text: "<kind> <segments> <length in metres>" ("whip 20 1.60"),
// then the settings that differ from the studio's first ones as "<code>=<value>" (the codes of
// physical_prop_setting below: "whip 20 1.60 p=20 c=2 d=0.1"), "" for none. Each client builds
// the prop on that player's body and ties its start to the player's hand, which moves with the
// player's synchronised animation; the rest of the rope swings by each client's own physics.
namespace kcd2s::integration {

// A chain is a rope drawn with the game's chain links.
enum class PhysicalPropKind { Rope, Whip, Chain };

struct PhysicalProp {
  PhysicalPropKind kind = PhysicalPropKind::Rope;
  int segments = 16;
  float length = 1.2f;  // metres; a whip's lash
  // The hand that holds it (h: 0 the right, 1 the left).
  bool left_hand = false;
  // A whip's grip (studio Grip angle and twist, degrees): p 0 points the handle down the
  // forearm, 90 out of the fist; t about the forearm.
  float pitch = 90.0f;
  float twist = 0.0f;
  // How the rope or lash moves (the studio's Physics mode, physical_props/rope_prop.hpp
  // RopePhysics): g gravity m/s², d damping, m mass kg, a air drag, b bend limit degrees (0
  // free), s stiffness (0 the game's own), c collide (1 bodies, props and ground; 2 the ground
  // only; 0 nothing).
  float gravity = 9.8f;
  float damping = 0.3f;
  float mass = 0.5f;
  float air = 0.1f;
  float bend = 0.0f;
  float stiffness = 0.0f;
  int collide = 1;
  // A whip's lash posed instead of moved by physics (physical_props/whip_lash.hpp): k shape (0
  // physics, 1 the posed shape, no gravity), u curl and w sway (degrees it bends from the handle
  // to the tip, toward the handle's up side and sideways). Prop keys blend them along an
  // animation, so a swing is animated exactly.
  float shape = 0.0f;
  float curl = 0.0f;
  float sway = 0.0f;
};

// One setting of a prop by its code: its range, whether it only takes whole steps (the hand and
// collide switch at a key instead of blending), and where it is in the prop. Null for a code
// that is none.
struct PhysicalPropSetting {
  char code;
  float low;
  float high;
  bool stepped;
};

inline const PhysicalPropSetting* physical_prop_setting(char code) {
  static constexpr PhysicalPropSetting kSettings[] = {
      {'h', 0.0f, 1.0f, true},       {'p', 0.0f, 180.0f, false}, {'t', -180.0f, 180.0f, false},
      {'g', 0.0f, 30.0f, false},     {'d', 0.0f, 5.0f, false},   {'m', 0.05f, 10.0f, false},
      {'a', 0.0f, 2.0f, false},      {'b', 0.0f, 90.0f, false},  {'s', 0.0f, 500.0f, false},
      {'c', 0.0f, 2.0f, true},       {'k', 0.0f, 1.0f, false},   {'u', -540.0f, 540.0f, false},
      {'w', -540.0f, 540.0f, false},
  };
  for (const PhysicalPropSetting& setting : kSettings) {
    if (setting.code == code) {
      return &setting;
    }
  }
  return nullptr;
}

// The prop's setting of that code (physical_prop_setting), as a number.
inline float physical_prop_value(const PhysicalProp& prop, char code) {
  switch (code) {
    case 'h':
      return prop.left_hand ? 1.0f : 0.0f;
    case 'p':
      return prop.pitch;
    case 't':
      return prop.twist;
    case 'g':
      return prop.gravity;
    case 'd':
      return prop.damping;
    case 'm':
      return prop.mass;
    case 'a':
      return prop.air;
    case 'b':
      return prop.bend;
    case 's':
      return prop.stiffness;
    case 'c':
      return float(prop.collide);
    case 'k':
      return prop.shape;
    case 'u':
      return prop.curl;
    case 'w':
      return prop.sway;
    default:
      return 0.0f;
  }
}

// Sets the prop's setting of that code (in its range; stepped ones rounded); false for a code
// that is none or a value that is not a number.
inline bool set_physical_prop_value(PhysicalProp& prop, char code, float value) {
  const PhysicalPropSetting* setting = physical_prop_setting(code);
  if (!setting || !std::isfinite(value)) {
    return false;
  }
  value = value < setting->low ? setting->low : value > setting->high ? setting->high : value;
  if (setting->stepped) {
    value = std::round(value);
  }
  switch (code) {
    case 'h':
      prop.left_hand = value >= 0.5f;
      break;
    case 'p':
      prop.pitch = value;
      break;
    case 't':
      prop.twist = value;
      break;
    case 'g':
      prop.gravity = value;
      break;
    case 'd':
      prop.damping = value;
      break;
    case 'm':
      prop.mass = value;
      break;
    case 'a':
      prop.air = value;
      break;
    case 'b':
      prop.bend = value;
      break;
    case 's':
      prop.stiffness = value;
      break;
    case 'c':
      prop.collide = int(value);
      break;
    case 'k':
      prop.shape = value;
      break;
    case 'u':
      prop.curl = value;
      break;
    case 'w':
      prop.sway = value;
      break;
    default:
      return false;
  }
  return true;
}

// The prop with an animation's prop values at a time (pose_animation.hpp pose_prop_at) on top.
inline PhysicalProp physical_prop_with(PhysicalProp prop, const std::map<char, float>& values) {
  for (const auto& [code, value] : values) {
    set_physical_prop_value(prop, code, value);
  }
  return prop;
}

inline constexpr char kPhysicalPropCodes[] = "hptgdmabsckuw";
inline constexpr std::size_t kMaxPhysicalPropField = 160;

inline constexpr int kMinPropSegments = 2;
inline constexpr int kMaxPropSegments = 64;
inline constexpr float kMinPropLength = 0.2f;
inline constexpr float kMaxPropLength = 20.0f;

inline const char* physical_prop_kind_name(PhysicalPropKind kind) {
  if (kind == PhysicalPropKind::Whip) {
    return "whip";
  }
  return kind == PhysicalPropKind::Chain ? "chain" : "rope";
}

inline std::optional<PhysicalPropKind> physical_prop_kind(const std::string& name) {
  if (name == "rope") {
    return PhysicalPropKind::Rope;
  }
  if (name == "whip") {
    return PhysicalPropKind::Whip;
  }
  if (name == "chain") {
    return PhysicalPropKind::Chain;
  }
  return std::nullopt;
}

inline bool physical_prop_fits(const PhysicalProp& prop) {
  return prop.segments >= kMinPropSegments && prop.segments <= kMaxPropSegments &&
         prop.length >= kMinPropLength && prop.length <= kMaxPropLength;
}

// The field's text of a prop: "whip 20 1.60", then each setting that differs from the first one
// as " <code>=<value>". The length is kept to a centimetre.
inline std::string physical_prop_field(const PhysicalProp& prop) {
  char text[48] = {};
  const char* kind = physical_prop_kind_name(prop.kind);
  std::snprintf(text, sizeof(text), "%s %d %.2f", kind, prop.segments, double(prop.length));
  std::string field = text;
  const PhysicalProp first;
  for (const char* code = kPhysicalPropCodes; *code; ++code) {
    const float value = physical_prop_value(prop, *code);
    if (value != physical_prop_value(first, *code)) {
      std::snprintf(text, sizeof(text), " %c=%g", *code, double(value));
      field += text;
    }
  }
  return field;
}

// The prop a field's text names; nullopt for "" (none) and for anything that does not fit.
inline std::optional<PhysicalProp> physical_prop_of(const std::string& field) {
  if (field.empty() || field.size() > kMaxPhysicalPropField) {
    return std::nullopt;
  }
  char kind[8] = {};
  int segments = 0;
  float length = 0.0f;
  int read = 0;
  if (std::sscanf(field.c_str(), "%7s %d %f%n", kind, &segments, &length, &read) != 3) {
    return std::nullopt;
  }
  const auto named = physical_prop_kind(kind);
  if (!named) {
    return std::nullopt;
  }
  PhysicalProp prop;
  prop.kind = *named;
  prop.segments = segments;
  prop.length = length;
  if (!physical_prop_fits(prop)) {
    return std::nullopt;
  }
  // The settings: " <code>=<value>" each.
  std::size_t at = std::size_t(read);
  while (at < field.size()) {
    if (field[at] != ' ' || at + 3 > field.size() || field[at + 2] != '=') {
      return std::nullopt;
    }
    const char code = field[at + 1];
    const char* start = field.c_str() + at + 3;
    char* end = nullptr;
    const float value = std::strtof(start, &end);
    const PhysicalPropSetting* setting = physical_prop_setting(code);
    if (end == start || !setting || value < setting->low || value > setting->high ||
        !set_physical_prop_value(prop, code, value)) {
      return std::nullopt;
    }
    at = std::size_t(end - field.c_str());
  }
  return prop;
}

}  // namespace kcd2s::integration
