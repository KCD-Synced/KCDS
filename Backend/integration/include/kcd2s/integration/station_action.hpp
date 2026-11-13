#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "kcd2s/integration/animation_props.hpp"

// What a player does at a crafting station, as every other client shows it
// (smithing): the owner's client publishes it in the NetworkPlayer field
// "stationAction" while its player works at a station, "" otherwise. The body's own animation
// travels as every animation does; this field carries what the animation holds and moves: the
// station's tools and the workpiece, where each one is, and how hot the forge and the workpiece
// are.
//
// The field: words separated by one space.
//   kind        "smith" (a smithy's anvil and forge)
//   S<x,y,z>    where the station stands (the game's Smithery entity), which every client finds
//               among its own entities of that class
//   W<id>       the workpiece the recipe forges ("horseshoe", "axe", "sword", "longsword"; the
//               game's BlacksmithWorkpieces), absent before the player chose a recipe
//   F<0..9>     how bright the forge's light burns, in thirds of the level's own light (0 dark,
//               3 as the level has it, 9 three times as bright)
//   G<hex>      how each heat zone of the workpiece glows (the game heats a workpiece in up to
//               four zones): per zone two hex digits, 00 cold to ff white hot (StationGlow's scale)
//   E<0..7>     the station's lasting effects that burn (StationEffect bits)
//   K<0..255>   the anvil's spark bursts so far (it wraps); every new one makes the sparks
//   Q<0..255>   the quench's steam bursts so far (it wraps); every new one makes the steam
//   A<hex x18>  the minigame's sounds (kStationSounds, in that order): for each, how often it was
//               started, then for each how often it was stopped, one hex digit each (they wrap at
//               16); every new start or stop does the same on the replica
//   Z<x,y,z,s>  where the last stroke's sparks flew, relative to the station, and how many (the
//               emitter's count scale: a perfect stroke throws more); absent before the first
//   B<s,...>    how strongly each of the station's overheated-iron effects burns (by their names'
//               order; 0 out), absent while none burns
//   O<x,y,z,...> where each of those effects is, relative to the workpiece, three numbers per
//               effect in the same order (the game moves them to the overheated part of the
//               piece), absent while none burns or without a workpiece
//   P<name,f>   the clip the bellows' own character plays (the forge bag's lever, which the
//               smith's animation drives as its slave object) and its start flags, absent while it
//               plays none: a blend space by its name in the bag's animation set, a plain clip
//               (which a release build keeps no name of) as "#<id>"; the lever's pull is the
//               body's motion parameters, which travel with the body
// and then each prop the owner's game moved, three words: its id and anchor ("HR": the hammer
// in the right hand), its position "x,y,z" in metres and its rotation "x,y,z,w" relative to the
// anchor: the hand's joint, or the station (its position and turn, without its scale).
// Ids: H the hammer, T the tongs, B the bellows' lever, W the workpiece. Anchors: R the right
// hand, L the left hand, S the station. A prop that is not in the field lies where the level put
// it.
//
// A station of kind "alchemy" is mirrored entity by entity instead: after the props
// come entries, each a word "@<key>" and the words that say how it differs from the level: "h"
// hidden, "v" shown, "p<x,y,z>" and "q<x,y,z,w>" its place relative to the station, "m<model>" the
// mesh it shows, "b<0|1>" whether its particles burn, "l<r,g,b>" its light's colour (the engine's,
// 0 for a light that is out, above 1 for a bright one), "s<count,strength>" its particles' spawn
// parameters (a fire burning up or down), "a<trigger.starts.stops,...>" the sounds the minigame
// played on it (the trigger's id in hex, how often started and stopped), "c<name,flags>" the clip
// its own character plays now as P names a clip (the recipe book opening, the bellows pumped),
// "r<R|L>" after p and q: the place is relative to the brewer's right or left
// hand joint instead of the station (a thing held in the hand),
// "u<parameter.value,...>" the audio parameters the minigame set on it (the parameter's id in
// hex, its value).
// A key is a level entity's name before its "[" (the station's own things: AlchemyItem_mortar,
// particle_fireplace_pot, light_fireplace_pot, ...) or "#<n>" for a thing the minigame spawned
// (herbs in the mortar or the bowl), which every other client spawns too, or "#fx<n>" for a
// particle effect the brewer's animation plays on a hand now (the pouring stream): m its effect's
// name, p and q its offset, r its hand, on that hand's item slot joint (Client/runtime/crafting/
// hand_effects.hpp). A thing not in the field is as the level has it.
//
// A station of kind "grind" (a grindstone) carries no props and keeps the smithy's
// words at rest (F0 G00 E0 K0 Q0 and no sounds): S is where the grindstone stands (the game's
// Grindstone entity), P the clip its wheel plays (grindstone_spinning_<suffix> on the
// grindstone's own character, which the game runs at the sharpener's own clip time), V<speed>
// how fast the wheel turns (0 at rest: the wheel and the pedalling stand still), Y<layer.scale,...>
// the playback scale of each of the sharpener's animation layers the minigame slowed or sped up
// (the pedalling follows the wheel; a layer at 1 is not listed), D<clip> the sharpener's pedalling
// clip as P names a clip (whose time the wheel follows), and its entries are fixed keys:
// "wheel" the grindstone itself with the sounds (a) and audio parameters (u) the minigame gave it,
// "sparks" and "fail" the minigame's two spark emitters at the wheel (its good and its bad contact)
// with their spawn parameters (s), absent while the game has none.
namespace kcd2s::integration {

inline constexpr char kStationKindSmith[] = "smith";
inline constexpr char kStationKindAlchemy[] = "alchemy";
inline constexpr char kStationKindGrind[] = "grind";
inline constexpr char kStationGrindWheel[] = "wheel";
inline constexpr char kStationGrindSparks[] = "sparks";
inline constexpr char kStationGrindFail[] = "fail";
inline constexpr std::size_t kStationEntriesMax = 48;
inline constexpr std::size_t kStationKeyMax = 48;
inline constexpr std::size_t kStationModelMax = 128;
// The brightest light colour an entry may give (the engine's units).
inline constexpr float kStationColourMax = 1000.0f;
inline constexpr std::size_t kStationWorkpieceMax = 32;
inline constexpr std::size_t kStationClipMax = 64;
inline constexpr std::size_t kStationPropsMax = 4;
// How far a prop may be from its anchor: a hand holds it close, a station's own things lie
// within its room.
inline constexpr float kStationHandReach = 2.0f;
inline constexpr float kStationRoomReach = 12.0f;
inline constexpr float kStationWorldReach = 100000.0f;
// The overheated-iron effects a station may have, and how strong an effect may be told to burn.
inline constexpr std::size_t kStationBurningMax = 8;
inline constexpr float kStationStrengthMax = 100.0f;
// How many particles an entry's emitter may be told to throw (a grindstone's sparks: up to 300).
inline constexpr float kStationSpawnMax = 1000.0f;

// The station's lasting effects (the field's E bits): the forge's flare as the fire is woken, its
// heat shimmer, and the smoke of an overheated workpiece.
enum StationEffect : std::uint8_t {
  kStationForgeFlare = 1,
  kStationForgeHeat = 2,
  kStationBurning = 4,
};
inline constexpr std::uint8_t kStationEffectsAll = 7;

// The smithing minigame's sounds (its audio triggers), in the order the field counts them.
inline constexpr const char* kStationSounds[] = {
    "f_bl_forge",
    "f_bl_bellow",
    "f_bl_bellows_pull",
    "f_bl_bellows_release",
    "f_bl_piece_move_in_forge",
    "f_bl_overburning",
    "f_bl_hammering",
    "f_bl_anvil_hammer_hit",
    "f_bl_piece_move_on_anvil",
};
inline constexpr std::size_t kStationSoundCount =
    sizeof(kStationSounds) / sizeof(kStationSounds[0]);
inline constexpr char kStationSoundsNone[] = "000000000000000000";

// A heat zone's glow: its temperature from 400 °C (level 0, the game's coldest glow) to 1600 °C
// (level 255, white), in equal steps of about 4.7 °C, so a cooling piece fades on every screen as
// it does on the owner's (ten steps of 133 °C showed it dark well before the owner's went out).
inline constexpr std::size_t kStationZonesMax = 4;
inline constexpr float kStationGlowCold = 400.0f;
inline constexpr float kStationGlowWhite = 1600.0f;
inline constexpr int kStationGlowLevels = 255;
inline constexpr float kStationGlowStep =
    (kStationGlowWhite - kStationGlowCold) / static_cast<float>(kStationGlowLevels);

inline int station_glow_level(float temperature) {
  float level = std::round((temperature - kStationGlowCold) / kStationGlowStep);
  level = level < 0.0f ? 0.0f : level;
  level = level > static_cast<float>(kStationGlowLevels) ? static_cast<float>(kStationGlowLevels)
                                                         : level;
  return static_cast<int>(level);
}

inline float station_glow_temperature(int level) {
  return kStationGlowCold + static_cast<float>(level) * kStationGlowStep;
}

// A zone's level as the field carries it (two lowercase hex digits), and back; -1 for anything
// else.
inline std::string station_glow_text(int level) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.push_back(kHex[(level >> 4) & 0xF]);
  text.push_back(kHex[level & 0xF]);
  return text;
}

inline int station_glow_parse(std::string_view text) {
  if (text.size() != 2) {
    return -1;
  }
  int level = 0;
  for (const char digit : text) {
    level <<= 4;
    if (digit >= '0' && digit <= '9') {
      level += digit - '0';
    } else if (digit >= 'a' && digit <= 'f') {
      level += digit - 'a' + 10;
    } else {
      return -1;
    }
  }
  return level;
}

enum class StationPropId : char { Hammer = 'H', Tongs = 'T', Bellows = 'B', Workpiece = 'W' };
// Tongs ('T'): the workpiece carried in the tongs, relative to the tongs (the game hangs it on
// their Assetslot, C_BlacksmithingWorkpieceAttachment).
enum class StationAnchor : char { RightHand = 'R', LeftHand = 'L', Station = 'S', Tongs = 'T' };

struct StationProp {
  StationPropId id = StationPropId::Hammer;
  StationAnchor anchor = StationAnchor::Station;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  bool operator==(const StationProp&) const = default;
};

// A sound the minigame played on a thing: the audio trigger (its id, the same on every client)
// and how often it was started and stopped (they wrap at 256).
struct StationSound {
  std::uint32_t trigger = 0;
  std::uint8_t starts = 0;
  std::uint8_t stops = 0;
  bool operator==(const StationSound&) const = default;
};
inline constexpr std::size_t kStationEntrySoundsMax = 8;

// An audio parameter the minigame set on a thing (its id, the same on every client) and its value.
struct StationParameter {
  std::uint32_t id = 0;
  float value = 0.0f;
  bool operator==(const StationParameter&) const = default;
};
inline constexpr std::size_t kStationEntryParametersMax = 8;
inline constexpr float kStationParameterMax = 100000.0f;

// One of the station's things as the owner's game shows it, where it differs from the level.
struct StationEntry {
  std::string key;
  std::int8_t hidden = -1;  // 1 hidden, 0 shown, -1 as the level has it
  bool placed = false;      // position and rotation given
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  std::string model;
  std::int8_t burns = -1;  // -1 not given
  bool lit = false;        // the light's colour given
  std::array<float, 3> colour{};
  bool spawning = false;  // its particles' count scale and strength given
  std::array<float, 2> spawn{1.0f, 1.0f};
  std::vector<StationSound> sounds;
  std::vector<StationParameter> parameters;
  std::string clip;  // "" none
  std::uint32_t clip_flags = 0;
  char hand = 0;  // 'R' or 'L': position and rotation relative to that hand joint; 0 the station
  bool operator==(const StationEntry&) const = default;
};

// One animation layer of the worker's body and its playback scale.
struct StationLayerScale {
  std::uint8_t layer = 0;
  float scale = 1.0f;
  bool operator==(const StationLayerScale&) const = default;
};
inline constexpr std::size_t kStationLayersMax = 16;
inline constexpr float kStationScaleMax = 100.0f;

struct StationAction {
  std::string kind;
  std::array<float, 3> station{};
  std::string workpiece;
  std::uint8_t forge = 0;
  std::string glow = "00";
  std::uint8_t effects = 0;
  std::uint8_t strokes = 0;
  std::uint8_t quenches = 0;
  std::string sounds = kStationSoundsNone;
  bool sparks_at = false;
  std::array<float, 4> sparks{};  // x, y, z relative to the station, count scale
  std::vector<float> burning;     // strengths, 0 out
  std::vector<float> burning_at;  // x, y, z relative to the workpiece per burning entry, or none
  std::string bellows_clip;       // "" none
  std::uint32_t bellows_flags = 0;
  bool turning = false;  // V given
  float wheel_speed = 0.0f;
  std::vector<StationLayerScale> layer_scales;
  std::string body_clip;  // D: "" none
  std::vector<StationProp> props;
  std::vector<StationEntry> entries;
  bool operator==(const StationAction&) const = default;
};

namespace station_action_detail {

inline bool finite(const float* values, std::size_t count, float reach) {
  for (std::size_t i = 0; i < count; ++i) {
    if (!std::isfinite(values[i]) || std::fabs(values[i]) > reach) {
      return false;
    }
  }
  return true;
}

inline bool workpiece_valid(std::string_view id) {
  if (id.empty() || id.size() > kStationWorkpieceMax) {
    return false;
  }
  for (const char c : id) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != '_') {
      return false;
    }
  }
  return true;
}

inline bool prop_id_valid(char id) {
  return id == 'H' || id == 'T' || id == 'B' || id == 'W';
}

inline bool anchor_valid(char anchor) {
  return anchor == 'R' || anchor == 'L' || anchor == 'S' || anchor == 'T';
}

inline bool prop_valid(const StationProp& prop) {
  if (!prop_id_valid(static_cast<char>(prop.id)) || !anchor_valid(static_cast<char>(prop.anchor))) {
    return false;
  }
  const float reach = prop.anchor == StationAnchor::Station ? kStationRoomReach : kStationHandReach;
  if (!finite(prop.position.data(), 3, reach) || !finite(prop.rotation.data(), 4, 1.01f)) {
    return false;
  }
  const float length = prop.rotation[0] * prop.rotation[0] + prop.rotation[1] * prop.rotation[1] +
                       prop.rotation[2] * prop.rotation[2] + prop.rotation[3] * prop.rotation[3];
  return length > 0.5f && length < 1.5f;
}

// Two hex digits for every sound: its starts, then its stops.
inline bool sounds_valid(std::string_view sounds) {
  if (sounds.size() != 2 * kStationSoundCount) {
    return false;
  }
  for (const char digit : sounds) {
    const bool number = digit >= '0' && digit <= '9';
    const bool letter = digit >= 'a' && digit <= 'f';
    if (!number && !letter) {
      return false;
    }
  }
  return true;
}

// Two hex digits per heat zone, one to kStationZonesMax zones.
inline bool glow_valid(std::string_view glow) {
  if (glow.empty() || glow.size() % 2 != 0 || glow.size() > 2 * kStationZonesMax) {
    return false;
  }
  for (std::size_t i = 0; i < glow.size(); i += 2) {
    if (station_glow_parse(glow.substr(i, 2)) < 0) {
      return false;
    }
  }
  return true;
}

// Printable ASCII without spaces, 1 to `most` characters; a key never starts with '@'.
inline bool text_valid(std::string_view text, std::size_t most) {
  if (text.empty() || text.size() > most) {
    return false;
  }
  for (const char c : text) {
    if (c <= ' ' || c > '~') {
      return false;
    }
  }
  return true;
}

inline bool clip_name_valid(std::string_view name);

inline bool entry_valid(const StationEntry& entry) {
  if (!text_valid(entry.key, kStationKeyMax) || entry.key[0] == '@' || entry.hidden < -1 ||
      entry.hidden > 1 || (!entry.model.empty() && !text_valid(entry.model, kStationModelMax)) ||
      entry.burns < -1 || entry.burns > 1 ||
      (entry.lit && !finite(entry.colour.data(), 3, kStationColourMax)) ||
      (entry.spawning && !finite(entry.spawn.data(), 2, kStationSpawnMax)) ||
      entry.sounds.size() > kStationEntrySoundsMax ||
      entry.parameters.size() > kStationEntryParametersMax ||
      (!entry.clip.empty() && !clip_name_valid(entry.clip)) ||
      (entry.clip.empty() && entry.clip_flags != 0) ||
      (entry.hand != 0 && entry.hand != 'R' && entry.hand != 'L') ||
      (entry.hand != 0 && !entry.placed)) {
    return false;
  }
  for (const StationSound& sound : entry.sounds) {
    if (sound.trigger == 0) {
      return false;
    }
  }
  for (const StationParameter& parameter : entry.parameters) {
    if (parameter.id == 0 || !finite(&parameter.value, 1, kStationParameterMax)) {
      return false;
    }
  }
  if (!entry.placed) {
    return true;
  }
  if (!finite(entry.position.data(), 3, kStationRoomReach) ||
      !finite(entry.rotation.data(), 4, 1.01f)) {
    return false;
  }
  const float length =
      entry.rotation[0] * entry.rotation[0] + entry.rotation[1] * entry.rotation[1] +
      entry.rotation[2] * entry.rotation[2] + entry.rotation[3] * entry.rotation[3];
  return length > 0.5f && length < 1.5f;
}

// A number word: the letter, then 0 to `most`.
inline bool parse_level(std::string_view word, char letter, unsigned most, std::uint8_t& value) {
  if (word.size() < 2 || word[0] != letter) {
    return false;
  }
  unsigned number = 0;
  const char* const end = word.data() + word.size();
  const auto read = std::from_chars(word.data() + 1, end, number);
  if (read.ec != std::errc() || read.ptr != end || number > most) {
    return false;
  }
  value = static_cast<std::uint8_t>(number);
  return true;
}

// An entry's sounds: "<trigger hex>.<starts>.<stops>", comma separated.
inline bool parse_sounds(std::string_view text, std::vector<StationSound>& sounds) {
  while (!text.empty()) {
    const std::size_t comma = text.find(',');
    const std::string_view one = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    if (comma != std::string_view::npos && text.empty()) {
      return false;
    }
    StationSound sound;
    unsigned starts = 0;
    unsigned stops = 0;
    const char* const end = one.data() + one.size();
    const auto trigger = std::from_chars(one.data(), end, sound.trigger, 16);
    if (trigger.ec != std::errc() || trigger.ptr == end || *trigger.ptr != '.') {
      return false;
    }
    const auto started = std::from_chars(trigger.ptr + 1, end, starts);
    if (started.ec != std::errc() || started.ptr == end || *started.ptr != '.') {
      return false;
    }
    const auto stopped = std::from_chars(started.ptr + 1, end, stops);
    if (stopped.ec != std::errc() || stopped.ptr != end || starts > 255 || stops > 255 ||
        sounds.size() >= kStationEntrySoundsMax) {
      return false;
    }
    sound.starts = static_cast<std::uint8_t>(starts);
    sound.stops = static_cast<std::uint8_t>(stops);
    sounds.push_back(sound);
  }
  return !sounds.empty();
}

// An entry's audio parameters: "<id hex>.<value>", comma separated.
inline bool parse_parameters(std::string_view text, std::vector<StationParameter>& parameters) {
  while (!text.empty()) {
    const std::size_t comma = text.find(',');
    const std::string_view one = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    if (comma != std::string_view::npos && text.empty()) {
      return false;
    }
    StationParameter parameter;
    const char* const end = one.data() + one.size();
    const auto id = std::from_chars(one.data(), end, parameter.id, 16);
    if (id.ec != std::errc() || id.ptr == end || *id.ptr != '.') {
      return false;
    }
    const auto value = std::from_chars(id.ptr + 1, end, parameter.value);
    if (value.ec != std::errc() || value.ptr != end ||
        parameters.size() >= kStationEntryParametersMax) {
      return false;
    }
    parameters.push_back(parameter);
  }
  return !parameters.empty();
}

// The body's layer scales: "<layer>.<scale>", comma separated, each layer once.
inline bool parse_layer_scales(std::string_view text, std::vector<StationLayerScale>& scales) {
  while (!text.empty()) {
    const std::size_t comma = text.find(',');
    const std::string_view one = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    if (comma != std::string_view::npos && text.empty()) {
      return false;
    }
    StationLayerScale scale;
    unsigned layer = 0;
    const char* const end = one.data() + one.size();
    const auto read_layer = std::from_chars(one.data(), end, layer);
    if (read_layer.ec != std::errc() || read_layer.ptr == end || *read_layer.ptr != '.' ||
        layer >= kStationLayersMax) {
      return false;
    }
    const auto value = std::from_chars(read_layer.ptr + 1, end, scale.scale);
    if (value.ec != std::errc() || value.ptr != end || scales.size() >= kStationLayersMax) {
      return false;
    }
    scale.layer = static_cast<std::uint8_t>(layer);
    scales.push_back(scale);
  }
  return !scales.empty();
}

// A clip's name in an animation set: letters, digits, '_' and '-', 1 to kStationClipMax of them;
// or "#" and the clip's id.
inline bool clip_name_valid(std::string_view name) {
  if (name.empty() || name.size() > kStationClipMax) {
    return false;
  }
  if (name[0] == '#') {
    if (name.size() < 2) {
      return false;
    }
    for (const char c : name.substr(1)) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    return true;
  }
  for (const char c : name) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

// A clip and its start flags: "<name>,<flags>", the flags decimal.
inline bool parse_clip(std::string_view text, std::string& clip, std::uint32_t& flags) {
  const std::size_t comma = text.rfind(',');
  if (comma == std::string_view::npos || !clip_name_valid(text.substr(0, comma))) {
    return false;
  }
  const std::string_view number = text.substr(comma + 1);
  const char* const end = number.data() + number.size();
  const auto read_flags = std::from_chars(number.data(), end, flags);
  if (number.empty() || read_flags.ec != std::errc() || read_flags.ptr != end) {
    return false;
  }
  clip = std::string(text.substr(0, comma));
  return true;
}

}  // namespace station_action_detail

inline bool station_action_valid(const StationAction& action) {
  if ((action.kind != kStationKindSmith && action.kind != kStationKindAlchemy &&
       action.kind != kStationKindGrind) ||
      !station_action_detail::finite(action.station.data(), 3, kStationWorldReach) ||
      action.entries.size() > kStationEntriesMax ||
      (!action.workpiece.empty() && !station_action_detail::workpiece_valid(action.workpiece)) ||
      action.forge > 9 || !station_action_detail::glow_valid(action.glow) ||
      action.effects > kStationEffectsAll || !station_action_detail::sounds_valid(action.sounds) ||
      action.props.size() > kStationPropsMax || action.burning.size() > kStationBurningMax ||
      (!action.bellows_clip.empty() &&
       !station_action_detail::clip_name_valid(action.bellows_clip)) ||
      (action.bellows_clip.empty() && action.bellows_flags != 0) ||
      (action.turning &&
       !station_action_detail::finite(&action.wheel_speed, 1, kStationScaleMax)) ||
      action.layer_scales.size() > kStationLayersMax ||
      (!action.body_clip.empty() && !station_action_detail::clip_name_valid(action.body_clip)) ||
      (action.sparks_at &&
       (!station_action_detail::finite(action.sparks.data(), 3, kStationRoomReach) ||
        !station_action_detail::finite(action.sparks.data() + 3, 1, kStationStrengthMax))) ||
      !station_action_detail::finite(action.burning.data(), action.burning.size(),
                                     kStationStrengthMax) ||
      (!action.burning_at.empty() && action.burning_at.size() != 3 * action.burning.size()) ||
      !station_action_detail::finite(action.burning_at.data(), action.burning_at.size(),
                                     kStationRoomReach)) {
    return false;
  }
  for (std::size_t i = 0; i < action.layer_scales.size(); ++i) {
    const StationLayerScale& scale = action.layer_scales[i];
    if (scale.layer >= kStationLayersMax ||
        !station_action_detail::finite(&scale.scale, 1, kStationScaleMax)) {
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (action.layer_scales[j].layer == scale.layer) {
        return false;
      }
    }
  }
  for (std::size_t i = 0; i < action.props.size(); ++i) {
    if (!station_action_detail::prop_valid(action.props[i])) {
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (action.props[j].id == action.props[i].id) {
        return false;
      }
    }
  }
  for (std::size_t i = 0; i < action.entries.size(); ++i) {
    if (!station_action_detail::entry_valid(action.entries[i])) {
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (action.entries[j].key == action.entries[i].key) {
        return false;
      }
    }
  }
  return true;
}

// The field of an action; "" for none, and for an action the field cannot carry.
inline std::string station_action_field(const StationAction& action) {
  if (!station_action_valid(action)) {
    return {};
  }
  std::string field = action.kind;
  field += " S";
  animation_props_detail::append_floats(field, action.station.data(), 3);
  if (!action.workpiece.empty()) {
    field += " W" + action.workpiece;
  }
  field += " F" + std::to_string(action.forge);
  field += " G" + action.glow;
  field += " E" + std::to_string(action.effects);
  field += " K" + std::to_string(action.strokes);
  field += " Q" + std::to_string(action.quenches);
  field += " A" + action.sounds;
  if (action.sparks_at) {
    field += " Z";
    animation_props_detail::append_floats(field, action.sparks.data(), 4);
  }
  if (!action.burning.empty()) {
    field += " B";
    animation_props_detail::append_floats(field, action.burning.data(), action.burning.size());
  }
  if (!action.burning_at.empty()) {
    field += " O";
    animation_props_detail::append_floats(field, action.burning_at.data(),
                                          action.burning_at.size());
  }
  if (!action.bellows_clip.empty()) {
    field += " P" + action.bellows_clip + "," + std::to_string(action.bellows_flags);
  }
  if (action.turning) {
    field += " V";
    animation_props_detail::append_floats(field, &action.wheel_speed, 1);
  }
  for (std::size_t i = 0; i < action.layer_scales.size(); ++i) {
    field += i == 0 ? " Y" : ",";
    field += std::to_string(action.layer_scales[i].layer) + ".";
    animation_props_detail::append_floats(field, &action.layer_scales[i].scale, 1);
  }
  if (!action.body_clip.empty()) {
    field += " D" + action.body_clip;
  }
  for (const auto& prop : action.props) {
    field.push_back(' ');
    field.push_back(static_cast<char>(prop.id));
    field.push_back(static_cast<char>(prop.anchor));
    field.push_back(' ');
    animation_props_detail::append_floats(field, prop.position.data(), 3);
    field.push_back(' ');
    animation_props_detail::append_floats(field, prop.rotation.data(), 4);
  }
  for (const auto& entry : action.entries) {
    field += " @" + entry.key;
    if (entry.hidden >= 0) {
      field += entry.hidden ? " h" : " v";
    }
    if (entry.placed) {
      field += " p";
      animation_props_detail::append_floats(field, entry.position.data(), 3);
      field += " q";
      animation_props_detail::append_floats(field, entry.rotation.data(), 4);
      if (entry.hand != 0) {
        field += " r";
        field.push_back(entry.hand);
      }
    }
    if (!entry.model.empty()) {
      field += " m" + entry.model;
    }
    if (entry.burns >= 0) {
      field += " b" + std::to_string(entry.burns);
    }
    if (entry.lit) {
      field += " l";
      animation_props_detail::append_floats(field, entry.colour.data(), 3);
    }
    if (entry.spawning) {
      field += " s";
      animation_props_detail::append_floats(field, entry.spawn.data(), 2);
    }
    for (std::size_t i = 0; i < entry.sounds.size(); ++i) {
      const StationSound& sound = entry.sounds[i];
      char text[32];
      std::snprintf(text, sizeof(text), "%s%08x.%u.%u", i == 0 ? " a" : ",",
                    static_cast<unsigned>(sound.trigger), static_cast<unsigned>(sound.starts),
                    static_cast<unsigned>(sound.stops));
      field += text;
    }
    for (std::size_t i = 0; i < entry.parameters.size(); ++i) {
      const StationParameter& parameter = entry.parameters[i];
      char text[24];
      std::snprintf(text, sizeof(text), "%s%08x.", i == 0 ? " u" : ",",
                    static_cast<unsigned>(parameter.id));
      field += text;
      animation_props_detail::append_floats(field, &parameter.value, 1);
    }
    if (!entry.clip.empty()) {
      field += " c" + entry.clip + "," + std::to_string(entry.clip_flags);
    }
  }
  return field;
}

// The action of a field; false (and none) for "" and for anything station_action_field does not
// write.
inline bool parse_station_action_field(const std::string& field, StationAction& action) {
  action = StationAction{};
  if (field.empty()) {
    return false;
  }
  std::vector<std::string_view> words;
  if (!animation_props_detail::split_words(field, words) || words.size() < 8) {
    return false;
  }
  StationAction parsed;
  parsed.kind = std::string(words[0]);
  std::size_t next = 1;
  if (words[next].size() < 2 || words[next][0] != 'S' ||
      !animation_props_detail::parse_floats(words[next].substr(1), parsed.station.data(), 3)) {
    return false;
  }
  ++next;
  if (words[next].size() > 1 && words[next][0] == 'W') {
    parsed.workpiece = std::string(words[next].substr(1));
    ++next;
  }
  if (next + 6 > words.size() ||
      !station_action_detail::parse_level(words[next], 'F', 9, parsed.forge) ||
      words[next + 1].size() < 2 || words[next + 1][0] != 'G' ||
      !station_action_detail::parse_level(words[next + 2], 'E', kStationEffectsAll,
                                          parsed.effects) ||
      !station_action_detail::parse_level(words[next + 3], 'K', 255, parsed.strokes) ||
      !station_action_detail::parse_level(words[next + 4], 'Q', 255, parsed.quenches) ||
      words[next + 5].size() < 2 || words[next + 5][0] != 'A') {
    return false;
  }
  parsed.glow = std::string(words[next + 1].substr(1));
  parsed.sounds = std::string(words[next + 5].substr(1));
  next += 6;
  // The optional words: longer than a prop's two letters.
  if (next < words.size() && words[next].size() > 2 && words[next][0] == 'Z') {
    if (!animation_props_detail::parse_floats(words[next].substr(1), parsed.sparks.data(), 4)) {
      return false;
    }
    parsed.sparks_at = true;
    ++next;
  }
  if (next < words.size() && words[next].size() > 2 && words[next][0] == 'B') {
    const std::string_view values = words[next].substr(1);
    const std::size_t count =
        static_cast<std::size_t>(std::count(values.begin(), values.end(), ',')) + 1;
    if (count > kStationBurningMax) {
      return false;
    }
    parsed.burning.resize(count);
    if (!animation_props_detail::parse_floats(values, parsed.burning.data(), count)) {
      return false;
    }
    ++next;
  }
  if (next < words.size() && words[next].size() > 2 && words[next][0] == 'O') {
    const std::string_view values = words[next].substr(1);
    const std::size_t count =
        static_cast<std::size_t>(std::count(values.begin(), values.end(), ',')) + 1;
    if (count > 3 * kStationBurningMax) {
      return false;
    }
    parsed.burning_at.resize(count);
    if (!animation_props_detail::parse_floats(values, parsed.burning_at.data(), count)) {
      return false;
    }
    ++next;
  }
  if (next < words.size() && words[next].size() > 2 && words[next][0] == 'P') {
    if (!station_action_detail::parse_clip(words[next].substr(1), parsed.bellows_clip,
                                           parsed.bellows_flags)) {
      return false;
    }
    ++next;
  }
  if (next < words.size() && words[next].size() > 2 && words[next][0] == 'V') {
    if (!animation_props_detail::parse_floats(words[next].substr(1), &parsed.wheel_speed, 1)) {
      return false;
    }
    parsed.turning = true;
    ++next;
  }
  if (next < words.size() && words[next].size() > 2 && words[next][0] == 'Y') {
    if (!station_action_detail::parse_layer_scales(words[next].substr(1), parsed.layer_scales)) {
      return false;
    }
    ++next;
  }
  if (next < words.size() && words[next].size() > 2 && words[next][0] == 'D') {
    parsed.body_clip = std::string(words[next].substr(1));
    ++next;
  }
  while (next < words.size() && words[next][0] != '@') {
    if (next + 3 > words.size() || words[next].size() != 2) {
      return false;
    }
    StationProp prop;
    prop.id = static_cast<StationPropId>(words[next][0]);
    prop.anchor = static_cast<StationAnchor>(words[next][1]);
    if (!animation_props_detail::parse_floats(words[next + 1], prop.position.data(), 3) ||
        !animation_props_detail::parse_floats(words[next + 2], prop.rotation.data(), 4)) {
      return false;
    }
    parsed.props.push_back(prop);
    next += 3;
  }
  while (next < words.size()) {
    if (words[next].size() < 2 || words[next][0] != '@') {
      return false;
    }
    StationEntry entry;
    entry.key = std::string(words[next].substr(1));
    ++next;
    while (next < words.size() && words[next][0] != '@') {
      const std::string_view word = words[next];
      const std::string_view value = word.substr(1);
      std::uint8_t level = 0;
      if (word == "h" || word == "v") {
        entry.hidden = word == "h" ? 1 : 0;
      } else if (word[0] == 'p' &&
                 animation_props_detail::parse_floats(value, entry.position.data(), 3)) {
        entry.placed = true;
      } else if (word[0] == 'q' && entry.placed &&
                 animation_props_detail::parse_floats(value, entry.rotation.data(), 4)) {
      } else if (word[0] == 'm' && !value.empty()) {
        entry.model = std::string(value);
      } else if (station_action_detail::parse_level(word, 'b', 1, level)) {
        entry.burns = static_cast<std::int8_t>(level);
      } else if (word[0] == 'l' &&
                 animation_props_detail::parse_floats(value, entry.colour.data(), 3)) {
        entry.lit = true;
      } else if (word[0] == 's' &&
                 animation_props_detail::parse_floats(value, entry.spawn.data(), 2)) {
        entry.spawning = true;
      } else if (word[0] == 'a' && entry.sounds.empty() &&
                 station_action_detail::parse_sounds(value, entry.sounds)) {
      } else if (word[0] == 'u' && entry.parameters.empty() &&
                 station_action_detail::parse_parameters(value, entry.parameters)) {
      } else if ((word == "rR" || word == "rL") && entry.placed && entry.hand == 0) {
        entry.hand = word[1];
      } else if (word[0] == 'c' && entry.clip.empty() &&
                 station_action_detail::parse_clip(value, entry.clip, entry.clip_flags)) {
      } else {
        return false;
      }
      ++next;
    }
    parsed.entries.push_back(std::move(entry));
  }
  if (!station_action_valid(parsed)) {
    return false;
  }
  action = std::move(parsed);
  return true;
}

// The model the game forges a workpiece in (Libs/Tables/minigame/BlacksmithWorkpieces.xml), or
// null for an id it does not have.
inline const char* station_workpiece_model(std::string_view id) {
  if (id == "horseshoe") {
    return "objects/characters/assets/blacksmithing/blacksmithing_horseshoe.cdf";
  }
  if (id == "axe") {
    return "objects/characters/assets/blacksmithing/blacksmithing_axe.cdf";
  }
  if (id == "sword") {
    return "objects/characters/assets/blacksmithing/blacksmithing_short_sword.cdf";
  }
  if (id == "longsword") {
    return "objects/characters/assets/blacksmithing/blacksmithing_long_sword.cdf";
  }
  return nullptr;
}

}  // namespace kcd2s::integration
