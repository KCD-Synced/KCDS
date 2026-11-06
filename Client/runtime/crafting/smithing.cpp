#include "smithing.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../animation/animation_props.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "MinHook.h"
#include "emitters.hpp"
#include "sounds.hpp"
#include "station_clips.hpp"
#include "kcd2s/bridge/sigscan.hpp"
#include "kcd2s/integration/guid.hpp"
#include "kcd2s/integration/station_action.hpp"

namespace kcds::runtime::crafting::smithing {
namespace {

namespace wire = kcd2s::integration;
namespace entity = props::entity;

// C_Blacksmithing and its context (see smithing.hpp).
constexpr char kMinigameType[] = ".?AVC_Blacksmithing@playermodule@wh@@";
constexpr std::size_t kUpdateSlot = 19;
constexpr std::size_t kActor = 24;
constexpr std::size_t kStation = 40;
constexpr std::size_t kRunning = 44;
constexpr std::size_t kContextHolder = 104;
constexpr std::size_t kContextSlot = 12;
// The recipe the player chose (+ 152, null before one is chosen) and its id's text (+ 8), found
// live with the horseshoe at Tachov.
constexpr std::size_t kRecipe = 152;
constexpr std::size_t kRecipeId = 8;
constexpr std::size_t kRecipeIdMax = 64;
// The minigame's state (E_BlacksmithState): a model property at + 128 of its context, its slot 1
// the value (the frame update reads it so); 18 BrokenWorkpiece, which the game turns
// to 21 Finished.
constexpr std::size_t kStateProperty = 128;
constexpr std::size_t kStateGetter = 1;
constexpr std::uint8_t kBrokenWorkpiece = 18;
// C_BlacksmithingAttachment.
constexpr std::size_t kAttachmentSlot = 8;
constexpr std::size_t kAttachmentEntity = 16;
constexpr std::size_t kAttachmentHangs = 20;

// The station and what lies at it, as every client's level has them.
constexpr char kStationClass[] = "Smithery";
constexpr char kItemClass[] = "PickableItem";
constexpr char kMeshClass[] = "GeomEntity";
constexpr char kHammerPrefix[] = "blacksmith_hammer";
constexpr char kTongsPrefix[] = "blacksmith_tongs";
constexpr char kBellowsPrefix[] = "forge_bag";

// The hammer's and the tongs' attachments hold the station's item slot (HammerSlot, TongsSlot),
// which stays where it stands; the item in it (`item`, by its name) is what the hand carries.
struct Part {
  wire::StationPropId id;
  std::size_t offset;  // in the minigame's context
  const char* item;    // null: the attachment's entity itself moves
};
constexpr Part kParts[] = {
    {wire::StationPropId::Hammer, 736, kHammerPrefix},
    {wire::StationPropId::Tongs, 744, kTongsPrefix},
    {wire::StationPropId::Bellows, 752, nullptr},
    {wire::StationPropId::Workpiece, 760, nullptr},
};
constexpr std::size_t kPartCount = sizeof(kParts) / sizeof(kParts[0]);
// How far from its slot a station's item is looked for as the session begins.
constexpr float kSlotReach = 1.0f;
// The item classes of the station's hammer and tongs (their models show the copies).
constexpr char kHammerClass[] = "0502824d-a654-4471-9978-c1624860dde1";
constexpr char kTongsClass[] = "f22b7bb9-fa73-4aa1-92e6-3943e2be7e69";
// How far the reported station may be from a client's own, and its tools from it.
constexpr float kStationMatch = 1.5f;
constexpr float kToolReach = 5.0f;

// The station's effects (smithing.hpp): their entities' classes and names, and the minigame's
// forge part.
constexpr char kParticleClass[] = "ParticleEffect";
constexpr char kLightClass[] = "Light";
constexpr char kFlarePrefix[] = "forge_particle_big";
constexpr char kHeatPrefix[] = "forge_particle[";
constexpr char kSparksPrefix[] = "hammering_hit_particle";
constexpr char kSteamPrefix[] = "hardening_particle";
constexpr char kBurningPrefix[] = "professions.blacksmith.burning_iron";
constexpr char kLightPrefix[] = "light_forge";
constexpr std::size_t kForgePart = 168;
constexpr std::size_t kForgeFlare = 60;
constexpr std::size_t kForgeHeat = 64;
constexpr std::size_t kForgeLight = 72;
constexpr std::size_t kForgeLightColour = 76;
// The minigame's anvil part (+ 176) and its sparks (+ 72): a stroke moves the sparks
// to where the hammer struck and gives their emitter a count scale by how well it struck.
constexpr std::size_t kAnvilPart = 176;
constexpr std::size_t kAnvilSparks = 72;
// A strength travels in twentieths.
constexpr float kStrengthStep = 0.05f;
// An overheated iron's smoke strength travels in hundredths, the step at which the game gives it
// again: in twentieths a weak smoke (below 0.025) went out on every other screen and a
// change smaller than 0.05 never reached them (no burn effect on other screens).
constexpr float kBurningStep = 0.01f;
// A particle component: its entity, whether it burns.
constexpr std::size_t kParticleEntity = 40;
constexpr std::size_t kParticleBurns = 108;
// A light entity's render nodes (CEntity slot 88 their count, 90 one by slot) and the node's
// light (slot 75), its colour at + 32.
constexpr std::size_t kSlotCount = 88;
constexpr std::size_t kSlotNode = 90;
constexpr std::size_t kNodeLight = 75;
constexpr std::size_t kLightColour = 32;
// The light's level travels in thirds of its own colour.
constexpr float kLightSteps = 3.0f;

// A heat zone's step changes once its heat is this many steps from the step it shows.
constexpr float kGlowHold = 0.75f;
// How often another client applies a changed glow to its copy (seconds).
constexpr double kGlowEvery = 0.5;
// The workpiece's heat zones (smithing.hpp).
constexpr std::size_t kWorkpiecePart = 760;
constexpr std::size_t kZonesBegin = 40;
constexpr std::size_t kZonesEnd = 48;
constexpr std::size_t kZoneIndex = 4;
constexpr std::size_t kZoneHeat = 8;
// IMaterial: its float and its Vec3 parameter set by name (the material effects' own calls).
constexpr std::size_t kMaterialFloat = 44;
constexpr std::size_t kMaterialVector = 45;

// Libs/Tables/minigame/BlacksmithTemperatures.xml: a temperature, its colour and, where the table
// gives one, the glow's intensity (between them the game blends).
struct Heat {
  float temperature;
  std::uint32_t colour;
  float intensity;  // < 0: none given
};
constexpr Heat kHeats[] = {
    {400.0f, 0x503e44, 0.0f},   {450.0f, 0x6e393b, 3.0f},   {700.0f, 0x9f393a, -1.0f},
    {760.0f, 0xe93830, 6.0f},   {790.0f, 0xf45f3e, -1.0f},  {815.0f, 0xf6813b, 20.0f},
    {850.0f, 0xf4883f, -1.0f},  {950.0f, 0xf9c534, -1.0f},  {1100.0f, 0xf6ff96, -1.0f},
    {1200.0f, 0xf6ff96, 45.0f}, {1400.0f, 0xfdffe3, -1.0f}, {1600.0f, 0xffffff, 60.0f},
};

constexpr char kRightHand[] = "RightHand";
constexpr char kLeftHand[] = "LeftHand";
// The attachments the game holds a hand's item on ("entity_right" in slot 0,
// "entity_left" in slot 1), and the actor's holder of its hand slots (IActor slot 194).
constexpr char kRightEntity[] = "entity_right";
constexpr char kLeftEntity[] = "entity_left";
constexpr std::size_t kHandSlots = 194;

// Without an update for this long the minigame has ended.
constexpr std::uint64_t kUpdateGraceMs = 500;
// The field changes at most this often while only places move.
constexpr double kPublishInterval = 0.1;
// A hand's offset is measured over this many frames, taking the one where the hand moved least
// (the attachment follows the hand a frame late while it swings).
constexpr int kSteadyFrames = 8;
// Once measured, the offset is taken again in every frame the hand moved less than this.
constexpr float kHandStill = 0.002f;
// The game holds the workpiece rigidly on its holder: an offset that changed less than this
// (metres, and quaternion dot from 1) since the frame before is the held one, not a frame the
// piece trailed its holder in.
constexpr float kOffsetSteady = 0.003f;
constexpr float kTurnSteady = 0.0005f;
// A tool whose origin is farther than this from the hand's joint is not in the hand (a swung
// hammer trails its hand by more than half a metre in a frame, live; the tongs in the forge lie
// two metres from the hand).
constexpr float kGripReach = 1.2f;
// The game's workpiece attachment (C_BlacksmithingWorkpieceAttachment): the piece hangs
// on the tongs' "Assetslot" (+ 32) while it is attached (+ 20, kAttachmentHangs).
constexpr std::size_t kWorkpieceOnTongs = 32;
// How far the carried workpiece may lie from the tongs item (its grip at their tip).
constexpr float kTongsReach = 1.0f;
// A place that moved less than this stays as published.
constexpr float kMoved = 0.005f;
constexpr float kTurned = 0.9998f;  // |dot| of the turns
// A copy is attached once its entity exists (the entity system finishes a spawn at the end of its
// frame); after this many frames it is given up and spawned again.
constexpr int kAttachFrames = 300;
// A copy at the station eases to a new place at this share per second, and is there once nearer
// than this.
constexpr double kEaseRate = 12.0;
// A station tool not found yet is looked for again this often (seconds).
constexpr double kLookAgain = 2.0;
constexpr float kSettled = 0.001f;

// SChildAttachParams: flag 4 attaches to a joint of the parent's character by name.
struct AttachParams {
  std::int32_t flags;
  const char* bone;
};
constexpr std::int32_t kAttachToJoint = 4;
using AttachChild = bool(__fastcall*)(void* parent, void* child, const AttachParams* params);
using Update = void(__fastcall*)(void* minigame);
using Get = void*(__fastcall*)(void* object);
using ParticleEnable = void*(__fastcall*)(void* component, bool on);
using ParticleComponent = void*(__fastcall*)(void* unused, void* entity);
using SlotCount = int(__fastcall*)(void* entity);
using SlotNode = void*(__fastcall*)(void* entity, int slot);

Natives natives;
Update original_update = nullptr;
ParticleEnable original_particle_enable = nullptr;

template <class T>
T read(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class... Args>
void log_line(const char* format, Args... args) {
  if (natives.log) {
    natives.log(format, args...);
  }
}

const char* joint_of(wire::StationAnchor anchor) {
  return anchor == wire::StationAnchor::LeftHand ? kLeftHand : kRightHand;
}

float distance(const float a[3], const float b[3]) {
  const float x = a[0] - b[0];
  const float y = a[1] - b[1];
  const float z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

// The bellows' pull and the reach into the forge (BlendWeight2, BlendWeight).
constexpr int kPullParameter = 11;
constexpr int kReachParameter = 10;
// ISkeletonAnim::SetLayerPlaybackScale (slot 23), which the binding sets with a start.
constexpr std::size_t kLayerPlaybackScale = 23;

// The bag's and the body's clips (station_clips.hpp).
using clips::character_of;
using clips::clip_id;
using clips::clip_name;
using clips::clip_parameter;
using clips::kLoopFlag;
using clips::kStartAtKeyTime;
using clips::skeleton_of;
using clips::top_clip;

// The particle component of a ParticleEffect entity, or null.
void* particle_of(void* thing) {
  if (!thing || !natives.particle_component) {
    return nullptr;
  }
  return reinterpret_cast<ParticleComponent>(natives.particle_component)(nullptr, thing);
}

bool burns(void* thing) {
  void* component = particle_of(thing);
  return component && read<bool>(component, kParticleBurns);
}

// Turns an effect on (a burst starts again) or off, as the minigame does.
void switch_effect(void* thing, bool on) {
  void* component = particle_of(thing);
  if (!component) {
    return;
  }
  if (on && natives.particle_activate) {
    reinterpret_cast<Get>(natives.particle_activate)(component);
  } else if (original_particle_enable) {
    original_particle_enable(component, on);
  }
}

// A burst set off again as the game's stroke does it: the effect restarted, then its
// new emitter given the stroke's count (3 on a perfect stroke, 0 on a weak one).
void restart_effect(void* thing, float count) {
  void* component = particle_of(thing);
  if (!component) {
    return;
  }
  if (!natives.particle_restart) {
    switch_effect(thing, true);
    return;
  }
  void* before = emitters::of(thing);
  reinterpret_cast<Get>(natives.particle_restart)(component);
  // Another client's sparks were never set off by its own minigame: no emitter and not marked to
  // start, so the restart left them out (slot -1, no burst on a perfect stroke). The game's
  // activation loads the effect into a slot first.
  if (!emitters::of(thing) && natives.particle_activate) {
    reinterpret_cast<Get>(natives.particle_activate)(component);
  }
  emitters::Given given;
  given.count = count;
  emitters::give(thing, given);
  log_line("SMITHING sparks restarted: emitter %p -> %p, burns %d, slot %d, starts %d", before,
           emitters::of(thing), int(burns(thing)), read<std::int32_t>(component, 104),
           int(read<std::uint8_t>(component, 109)));
}

// The colour of a light entity's light (its first render node that has one), or null.
float* light_colour(void* thing) {
  if (!thing) {
    return nullptr;
  }
  const int count = slot_of<SlotCount>(thing, kSlotCount)(thing);
  for (int i = 0; i < count; ++i) {
    void* node = slot_of<SlotNode>(thing, kSlotNode)(thing, i);
    if (!node) {
      continue;
    }
    auto* light = static_cast<std::uint8_t*>(slot_of<Get>(node, kNodeLight)(node));
    if (light) {
      return reinterpret_cast<float*>(light + kLightColour);
    }
  }
  return nullptr;
}

bool starts_with(const std::string& text, const char* prefix) {
  return text.rfind(prefix, 0) == 0;
}

// The audio trigger id of one of the minigame's sounds, 0 while the audio system has none.
std::uint32_t sound_id(std::size_t sound) {
  return sounds::trigger_id(wire::kStationSounds[sound]);
}

// Starts or stops one of the minigame's sounds on an entity.
void sound_on(void* thing, std::size_t sound, bool start) {
  sounds::play(thing, sound_id(sound), start);
}

// The colour (0..1) a temperature glows in, blended between the table's rows.
void heat_colour(float temperature, float colour[3]) {
  constexpr std::size_t count = sizeof(kHeats) / sizeof(kHeats[0]);
  std::size_t upper = 0;
  while (upper < count && kHeats[upper].temperature < temperature) {
    ++upper;
  }
  const Heat& high = kHeats[upper < count ? upper : count - 1];
  const Heat& low = kHeats[upper > 0 ? upper - 1 : 0];
  const float span = high.temperature - low.temperature;
  float t = span > 0.0f ? (temperature - low.temperature) / span : 0.0f;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  for (int channel = 0; channel < 3; ++channel) {
    const int shift = 16 - 8 * channel;
    const float a = static_cast<float>((low.colour >> shift) & 0xFF);
    const float b = static_cast<float>((high.colour >> shift) & 0xFF);
    colour[channel] = (a + (b - a) * t) / 255.0f;
  }
}

// The glow's intensity at a temperature, blended between the rows that give one.
float heat_intensity(float temperature) {
  const Heat* low = nullptr;
  const Heat* high = nullptr;
  for (const Heat& row : kHeats) {
    if (row.intensity < 0.0f) {
      continue;
    }
    if (row.temperature <= temperature) {
      low = &row;
    } else if (!high) {
      high = &row;
    }
  }
  if (!low) {
    return high ? high->intensity : 0.0f;
  }
  if (!high) {
    return low->intensity;
  }
  const float t = (temperature - low->temperature) / (high->temperature - low->temperature);
  return low->intensity + (high->intensity - low->intensity) * t;
}

using PartMaterial = bool(__fastcall*)(void* unused, void* entity, void* unused_too,
                                       const char* part, const char* sub_material, void** out);
using MaterialFloat = bool(__fastcall*)(void* material, const char* name, float value);
using MaterialVector = bool(__fastcall*)(void* material, const char* name, const float* value);

// Makes the copy's heat zone `zone` (0 the first) glow at `level` (StationGlow's scale), as the
// minigame makes its own workpiece glow: through the minigame's own zone glow when it was found.

void glow_zone(void* thing, std::vector<std::array<std::uint8_t, 64>>& zones, std::size_t zone,
               int level) {
  if (!thing) {
    return;
  }
  if (natives.zone_glow) {
    while (zones.size() <= zone) {
      zones.emplace_back();
      zones.back().fill(0);
    }
    std::uint8_t* object = zones[zone].data();
    const std::int32_t index = static_cast<std::int32_t>(zone);
    const float temperature = kcd2s::integration::station_glow_temperature(level);
    std::memcpy(object + kZoneIndex, &index, sizeof(index));
    std::memcpy(object + kZoneHeat, &temperature, sizeof(temperature));
    using ZoneGlow = void(__fastcall*)(void* zone, void* entity);
    reinterpret_cast<ZoneGlow>(natives.zone_glow)(object, thing);
    return;
  }
  if (!natives.part_material) {
    return;
  }
  char part[16];
  std::snprintf(part, sizeof(part), "hot_%02u", static_cast<unsigned>(zone + 1));
  void* material = nullptr;
  if (!reinterpret_cast<PartMaterial>(natives.part_material)(nullptr, thing, nullptr, part, part,
                                                             &material) ||
      !material) {
    log_line("SMITHING the workpiece has no part %s", part);
    return;
  }
  const float temperature = kcd2s::integration::station_glow_temperature(level);
  float colour[3];
  heat_colour(temperature, colour);
  const float intensity = level == 0 ? 0.0f : heat_intensity(temperature);
  const bool coloured =
      slot_of<MaterialVector>(material, kMaterialVector)(material, "emissive_color", colour);
  const bool lit =
      slot_of<MaterialFloat>(material, kMaterialFloat)(material, "emissive_intensity", intensity);
  if (!coloured || !lit) {
    log_line("SMITHING the workpiece's %s took colour %d intensity %d", part, int(coloured),
             int(lit));
  }
}

bool same_place(const wire::StationProp& a, const wire::StationProp& b) {
  if (a.id != b.id || a.anchor != b.anchor ||
      distance(a.position.data(), b.position.data()) > kMoved) {
    return false;
  }
  const float dot = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] +
                    a.rotation[2] * b.rotation[2] + a.rotation[3] * b.rotation[3];
  return std::fabs(dot) >= kTurned;
}

// ---------------------------------------------------------------------------------------------
// The owner's side.

// The minigame the local player's game runs, as its update last saw it.
void* local_actor = nullptr;
void* local_body = nullptr;
void* minigame = nullptr;
std::uint64_t minigame_seen = 0;
// The action the owner's client last published.
wire::StationAction published;

void __fastcall hooked_update(void* object) {
  original_update(object);
  if (object && local_actor && read<void*>(object, kActor) == local_actor) {
    minigame = object;
    minigame_seen = GetTickCount64();
  }
}

bool smithing_now() {
  return minigame && GetTickCount64() - minigame_seen <= kUpdateGraceMs;
}

// The bursts the local player's game set off at its station while it smiths.
std::uint8_t sparks = 0;
std::uint8_t steam = 0;
bool struck = false;
std::string last_switched;

void* __fastcall hooked_particle_enable(void* component, bool on) {
  void* result = original_particle_enable(component, on);
  if (!smithing_now() || !component) {
    return result;
  }
  void* thing = read<void*>(component, kParticleEntity);
  const std::string name = thing ? entity::name_of(thing, 64) : std::string();
  if (on && starts_with(name, kSparksPrefix)) {
    ++sparks;
    struck = true;
  } else if (on && starts_with(name, kSteamPrefix)) {
    ++steam;
  }
  const std::string switched = name + (on ? " on" : " off");
  if (switched != last_switched) {
    last_switched = switched;
    log_line("SMITHING own effect %s", switched.c_str());
  }
  return result;
}

// A strength as the field carries it: in steps, so its small changes do not change the field.
float in_steps(float value, float step = kStrengthStep) {
  const float stepped = std::round(value / step) * step;
  return stepped < 0.0f
             ? 0.0f
             : (stepped > wire::kStationStrengthMax ? wire::kStationStrengthMax : stepped);
}

// The minigame's sounds the local player's game started and stopped on its body (crafting
// sounds watches the body's audio component).
std::array<std::uint8_t, wire::kStationSoundCount> sound_starts{};
std::array<std::uint8_t, wire::kStationSoundCount> sound_stops{};
std::array<std::uint32_t, wire::kStationSoundCount> sound_ids{};

// Which of the minigame's sounds a trigger is, or kStationSoundCount.
std::size_t sound_of(std::uint32_t trigger) {
  for (std::size_t i = 0; i < wire::kStationSoundCount; ++i) {
    if (sound_ids[i] == 0) {
      sound_ids[i] = sound_id(i);
    }
    if (sound_ids[i] != 0 && sound_ids[i] == trigger) {
      return i;
    }
  }
  return wire::kStationSoundCount;
}

// Counts the minigame's sounds the game started and stopped on the body since the last frame;
// counted from the minigame's start on (its forge's roar begins before its first update): these
// sounds play only while the player smiths.
void count_sounds(void* body) {
  void* audio = sounds::audio_of(body);
  sounds::watch(audio);
  static std::vector<sounds::Heard> heard;
  sounds::take(audio, heard);
  for (const sounds::Heard& one : heard) {
    const std::size_t sound = sound_of(one.trigger);
    if (sound >= wire::kStationSoundCount) {
      continue;
    }
    if (one.start) {
      ++sound_starts[sound];
    } else {
      ++sound_stops[sound];
    }
  }
}

// The sounds' counts as the field carries them.
std::string sound_counts() {
  constexpr char kHex[] = "0123456789abcdef";
  std::string counts;
  for (const std::uint8_t starts : sound_starts) {
    counts.push_back(kHex[starts & 0xF]);
  }
  for (const std::uint8_t stops : sound_stops) {
    counts.push_back(kHex[stops & 0xF]);
  }
  return counts;
}

// The station's smoke of an overheated workpiece, found once a session.
std::vector<std::uint32_t> own_burning;
bool looked_for_burning = false;

// Every entity of a class within `reach` of a place whose name starts with `prefix`.
std::vector<std::uint32_t> all_near(const char* entity_class, const char* prefix,
                                    const float place[3], float reach) {
  std::vector<std::uint32_t> found;
  entity::each_of_class(entity_class, [&](void* candidate) {
    if (!starts_with(entity::name_of(candidate, 96), prefix)) {
      return;
    }
    float there[3];
    entity::world_position(candidate, there);
    if (distance(there, place) <= reach) {
      found.push_back(entity::id_of(candidate));
    }
  });
  return found;
}

// Entities in the order of their places (west to east, then south to north, then upwards): the
// same on every client, as every client has the same level.
void by_place(std::vector<std::uint32_t>& ids) {
  std::sort(ids.begin(), ids.end(), [](std::uint32_t a, std::uint32_t b) {
    float here[3] = {};
    float there[3] = {};
    if (void* thing = entity::by_id(a)) {
      entity::world_position(thing, here);
    }
    if (void* thing = entity::by_id(b)) {
      entity::world_position(thing, there);
    }
    return std::lexicographical_compare(here, here + 3, there, there + 3);
  });
}

// The glow the owner's zones were last given, each zone's step held until its heat is well into
// another. Held against what was read before, not what was published: the field goes out at most
// ten times a second, so a hold against it let a heat on a step's edge flip between two steps
// every frame and the other screens took whichever was current ("001", "000", "001" within
// 15 ms: the blade's glow flickered).
std::string held_glow;

// How each heat zone of the workpiece glows now, two hex digits per zone in the zones' order.
std::string read_glow(const std::uint8_t* context) {
  const auto* workpiece = read<const std::uint8_t*>(context, kWorkpiecePart);
  if (!workpiece) {
    return "00";
  }
  const auto* begin = read<void* const*>(workpiece, kZonesBegin);
  const auto* end = read<void* const*>(workpiece, kZonesEnd);
  std::string glow(2 * wire::kStationZonesMax, '0');
  std::size_t zones = 0;
  for (const auto* at = begin; at && at != end; ++at) {
    const auto* zone = static_cast<const std::uint8_t*>(*at);
    const std::int32_t index = zone ? read<std::int32_t>(zone, kZoneIndex) : -1;
    if (index < 0 || index >= static_cast<std::int32_t>(wire::kStationZonesMax)) {
      continue;
    }
    // A zone keeps its step until its heat is well into another, so a heat on a step's edge
    // does not flicker between two.
    const float heat = read<float>(zone, kZoneHeat);
    const std::size_t offset = 2 * static_cast<std::size_t>(index);
    const int before =
        offset + 2 <= held_glow.size() ? wire::station_glow_parse(held_glow.substr(offset, 2)) : 0;
    const float off =
        std::fabs(heat - wire::station_glow_temperature(before)) / wire::kStationGlowStep;
    const int level = off < kGlowHold ? before : wire::station_glow_level(heat);
    glow.replace(offset, 2, wire::station_glow_text(level));
    zones = std::max<std::size_t>(zones, index + 1);
  }
  glow.resize(2 * (zones > 0 ? zones : 1));
  held_glow = glow;
  return glow;
}

// The station's effects as the local player's game shows them now.
void read_effects(const std::uint8_t* context, const float station_position[3],
                  const float station_rotation[4], wire::StationAction& action) {
  action.glow = read_glow(context);
  action.sounds = sound_counts();
  static std::string logged_glow;
  if (action.glow != logged_glow) {
    logged_glow = action.glow;
    log_line("SMITHING own glow %s", action.glow.c_str());
  }
  action.forge = static_cast<std::uint8_t>(kLightSteps);
  action.strokes = sparks;
  action.quenches = steam;
  const auto* forge = read<const std::uint8_t*>(minigame, kForgePart);
  if (forge) {
    if (burns(entity::by_id(read<std::uint32_t>(forge, kForgeFlare)))) {
      action.effects |= wire::kStationForgeFlare;
    }
    if (burns(entity::by_id(read<std::uint32_t>(forge, kForgeHeat)))) {
      action.effects |= wire::kStationForgeHeat;
    }
    const float* colour = light_colour(entity::by_id(read<std::uint32_t>(forge, kForgeLight)));
    const float own = read<float>(forge, kForgeLightColour);
    if (colour && own > 0.0f) {
      const float level = std::round(colour[0] / own * kLightSteps);
      action.forge = static_cast<std::uint8_t>(level < 0.0f ? 0.0f : (level > 9.0f ? 9.0f : level));
    }
  }
  if (!looked_for_burning) {
    looked_for_burning = true;
    own_burning = all_near(kParticleClass, kBurningPrefix, station_position, kToolReach);
    by_place(own_burning);
  }
  // Each overheated iron's smoke and how strongly it burns (the workpiece's update gives its
  // emitter a strength as the iron burns).
  std::vector<float> strengths;
  std::vector<float> places;
  const auto* piece_part = read<const std::uint8_t*>(context, kWorkpiecePart);
  void* piece =
      piece_part ? entity::by_id(read<std::uint32_t>(piece_part, kAttachmentEntity)) : nullptr;
  for (const std::uint32_t id : own_burning) {
    void* thing = entity::by_id(id);
    float strength = 0.0f;
    // Where it burns, relative to the piece: the game moves these effects to the overheated part
    // of the piece (the white sparkle at the horseshoe's end on the owner's screen, none on
    // the others; placed against the station it lagged behind the carried piece).
    float place[3] = {};
    if (thing && piece) {
      float world[3];
      float turn[4];
      float piece_position[3];
      float piece_rotation[4];
      float relative_turn[4];
      entity::world_pose(thing, world, turn);
      entity::world_pose(piece, piece_position, piece_rotation);
      pose_math::relative(piece_position, piece_rotation, world, turn, place, relative_turn);
      for (float& axis : place) {
        axis = std::round(axis * 100.0f) / 100.0f;
      }
    }
    places.insert(places.end(), place, place + 3);
    if (burns(thing)) {
      action.effects |= wire::kStationBurning;
      void* emitter = emitters::of(thing);
      emitters::watch(emitter);
      emitters::Given given;
      const float now = emitters::given(emitter, given) ? given.strength : 1.0f;
      // A strength keeps its step until it is a whole step away, so it does not flicker.
      const std::size_t index = strengths.size();
      const bool had = index < published.burning.size() && published.burning[index] > 0.0f;
      strength = had && std::fabs(now - published.burning[index]) < kBurningStep
                     ? published.burning[index]
                     : in_steps(now, kBurningStep);
      // A smoke that burns is never told as out (0 is out on the other screens).
      if (strength < kBurningStep) {
        strength = kBurningStep;
      }
    }
    strengths.push_back(strength);
  }
  if ((action.effects & wire::kStationBurning) != 0 &&
      strengths.size() <= wire::kStationBurningMax) {
    action.burning = std::move(strengths);
    if (piece) {
      action.burning_at = std::move(places);
    }
  }
  // Where the last stroke's sparks flew (a stroke moves them to where the hammer struck)
  // and how many it threw.
  const auto* anvil = read<const std::uint8_t*>(minigame, kAnvilPart);
  void* anvil_sparks = anvil ? entity::by_id(read<std::uint32_t>(anvil, kAnvilSparks)) : nullptr;
  if (anvil_sparks && struck) {
    void* emitter = emitters::of(anvil_sparks);
    emitters::watch(emitter);
    float there[3];
    float turn[4];
    float rotation[4];
    entity::world_pose(anvil_sparks, there, turn);
    pose_math::relative(station_position, station_rotation, there, turn, action.sparks.data(),
                        rotation);
    emitters::Given given;
    const bool seen = emitters::given(emitter, given);
    action.sparks[3] = in_steps(seen ? given.count : 1.0f);
    action.sparks_at = distance(there, station_position) <= wire::kStationRoomReach;
    static std::uint8_t logged_strokes = 0;
    if (sparks != logged_strokes) {
      logged_strokes = sparks;
      log_line("SMITHING own stroke %u sparks at %.2f,%.2f,%.2f x%.3f (%s)", unsigned(sparks),
               action.sparks[0], action.sparks[1], action.sparks[2], seen ? given.count : -1.0f,
               seen ? "given" : "not seen");
    }
  }
}

// What the owner's client measured of one part.
struct Measured {
  wire::StationAnchor anchor = wire::StationAnchor::Station;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  int samples = 0;
  float least_motion = 0.0f;
  float last_joint[3] = {};
  bool settled = false;
  // The workpiece's offset from its holder in the frame before.
  std::array<float, 3> last_position{};
  std::array<float, 4> last_rotation{0.0f, 0.0f, 0.0f, 1.0f};
};

std::array<std::optional<Measured>, kPartCount> measured;
std::string published_field;
double published_at = -1.0;
// The recipe the local player chose this session, and one chosen since the last look.
std::string chosen_recipe;
std::string recipe_to_report;
std::array<float, 3> recipe_station{};
// The minigame's state at the last look, and a broken workpiece not yet reported.
std::uint8_t last_state = 0;
bool broken_to_report = false;

// The minigame's own lines for the smith (BLACKSMITHING_COLD_WORKPIECE and the rest: Henry's
// "I'll never finish it like this"): its bark component (minigame + 184, filled from
// the frame update) keeps a std::map at + 16 of kind -> { name, cooldown (float),
// last spoken (int64) }; a node's value sits at + 32, the cooldown at + 48, the last spoken at
// + 56 (the node: left + 0, parent + 8, right + 16, nil flag + 25). On a server the smith stays
// silent: every line is made never due again. Read under a fault guard.
constexpr std::size_t kBarkComponent = 184;
constexpr std::size_t kBarkMap = 16;
constexpr std::size_t kNodeLeft = 0;
constexpr std::size_t kNodeParent = 8;
constexpr std::size_t kNodeRight = 16;
constexpr std::size_t kNodeIsNil = 25;
constexpr std::size_t kNodeCooldown = 48;
constexpr std::size_t kNodeLastSpoken = 56;
constexpr std::size_t kMaxBarks = 32;

void silence_barks(void* minigame) {
  __try {
    auto* barks =
        *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(minigame) + kBarkComponent);
    if (!barks) {
      return;
    }
    auto* head = *reinterpret_cast<std::uint8_t**>(barks + kBarkMap);
    if (!head) {
      return;
    }
    std::uint8_t* pending[kMaxBarks] = {};
    std::size_t count = 0;
    std::size_t visited = 0;
    pending[count++] = *reinterpret_cast<std::uint8_t**>(head + kNodeParent);
    while (count > 0 && visited < kMaxBarks) {
      std::uint8_t* node = pending[--count];
      if (!node || node == head || node[kNodeIsNil]) {
        continue;
      }
      ++visited;
      *reinterpret_cast<float*>(node + kNodeCooldown) = 3.4e38f;
      *reinterpret_cast<std::int64_t*>(node + kNodeLastSpoken) = 0x3FFFFFFFFFFFFFFFll;
      if (count + 2 <= kMaxBarks) {
        pending[count++] = *reinterpret_cast<std::uint8_t**>(node + kNodeLeft);
        pending[count++] = *reinterpret_cast<std::uint8_t**>(node + kNodeRight);
      }
    }
  } __except (1) {
  }
}

// Copies the chosen recipe's id (the game's "r_...", or a resource's row of the table),
// when the minigame has one; false otherwise. Read under a fault guard: the recipe is the game's
// and only looked at; an id is letters, digits and underscores.
bool copy_recipe_id(const void* object, char* out, std::size_t size) {
  __try {
    const auto* recipe = *reinterpret_cast<const std::uint8_t* const*>(
        static_cast<const std::uint8_t*>(object) + kRecipe);
    if (!recipe) {
      return false;
    }
    const char* id = *reinterpret_cast<const char* const*>(recipe + kRecipeId);
    if (!id) {
      return false;
    }
    for (std::size_t i = 0; i < size; ++i) {
      const char c = id[i];
      out[i] = c;
      if (c == 0) {
        return i >= 2;
      }
      const bool word =
          (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      if (!word) {
        return false;
      }
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::string recipe_id_of(const void* object) {
  char id[kRecipeIdMax] = {};
  return copy_recipe_id(object, id, sizeof(id)) ? std::string(id) : std::string();
}
// The items the hammer's and the tongs' slots hold, and when they were last looked for.
std::array<std::uint32_t, kPartCount> own_items{};
std::array<std::uint64_t, kPartCount> items_looked_at{};

// What a part's attachment moves: the item in its slot (the station's tool nearest the slot as
// the session begins), or the attachment's entity itself.
void* moved_by(std::size_t index, void* slot) {
  if (!kParts[index].item) {
    return slot;
  }
  if (void* item = entity::by_id(own_items[index])) {
    return item;
  }
  const std::uint64_t tick = GetTickCount64();
  if (tick - items_looked_at[index] < static_cast<std::uint64_t>(kLookAgain * 1000.0)) {
    return nullptr;
  }
  items_looked_at[index] = tick;
  float at[3];
  entity::world_position(slot, at);
  float best = kSlotReach;
  void* found = nullptr;
  for (const std::uint32_t id : all_near(kItemClass, kParts[index].item, at, kSlotReach)) {
    void* candidate = entity::by_id(id);
    float there[3];
    entity::world_position(candidate, there);
    if (distance(there, at) <= best) {
      best = distance(there, at);
      found = candidate;
    }
  }
  own_items[index] = found ? entity::id_of(found) : 0;
  log_line("SMITHING own %c is the item %u %s", char(kParts[index].id), own_items[index],
           found ? entity::name_of(found, 48).c_str() : "(none)");
  return found;
}

// The owner's bellows, logged when their clip changes: the pull the bag's blend space plays and
// the one the smith's own plays.
void log_bellows(void* bag, const std::string& clip) {
  static std::string logged_clip = "-";
  if (clip == logged_clip) {
    return;
  }
  logged_clip = clip;
  float bag_pull = -1.0f;
  float body_pull = -1.0f;
  clip_parameter(skeleton_of(bag), kPullParameter, bag_pull);
  clip_parameter(skeleton_of(local_body), kPullParameter, body_pull);
  log_line("SMITHING own bellows clip %s pull %.2f body pull %.2f", clip.c_str(), bag_pull,
           body_pull);
}

// The carried workpiece while it lies still against the station: hammered on the
// anvil, held in the forge or in the trough. On the smith's screen the minigame keeps it there
// while the hands work about it; hung from the replica's hand it followed every swing of the
// hammering arm on the other screens and jumped between two places with each blow (owner's
// video). It is published against the station once it stayed still for kRestHold, and against its
// holder again once it left that place by more than kRestLeave (taken to the forge or the trough;
// the minigame moves it in and out of the fire by half a metre, live, which stays at the station
// and glides there on the other screens).
constexpr double kRestHold = 0.3;
// Still: within this far of where it stood when it stopped (a smith walking slowly moves it less
// than a centimetre a frame, live; measured frame to frame it counted as still).
constexpr float kRestStill = 0.02f;
constexpr float kRestLeave = 0.6f;

struct StationRest {
  bool on = false;
  // Where against the station it came to rest, and where it stood when it last stopped moving.
  std::array<float, 3> entered{};
  std::array<float, 3> stopped_at{};
  double still_since = -1.0;
};

StationRest workpiece_rest;

// Whether the carried workpiece lies still against the station; puts its place there into the prop
// while it does.
bool rests_on_station(const float station_position[3], const float station_rotation[4],
                      const float thing_position[3], const float thing_rotation[4], double now,
                      wire::StationProp& prop) {
  std::array<float, 3> local{};
  std::array<float, 4> turn{0.0f, 0.0f, 0.0f, 1.0f};
  pose_math::relative(station_position, station_rotation, thing_position, thing_rotation,
                      local.data(), turn.data());
  StationRest& rest = workpiece_rest;
  if (rest.still_since < 0.0 || distance(local.data(), rest.stopped_at.data()) > kRestStill) {
    rest.still_since = now;
    rest.stopped_at = local;
  }
  if (!rest.on && now - rest.still_since >= kRestHold) {
    rest.on = true;
    rest.entered = local;
    log_line("SMITHING own W at rest against the station");
  } else if (rest.on && distance(local.data(), rest.entered.data()) > kRestLeave) {
    rest.on = false;
    log_line("SMITHING own W left its place at the station");
  }
  if (!rest.on) {
    return false;
  }
  prop.anchor = wire::StationAnchor::Station;
  prop.position = local;
  prop.rotation = turn;
  pose_math::normalize(prop.rotation.data());
  return true;
}

void clear_owner() {
  held_glow.clear();
  measured = {};
  workpiece_rest = {};
  own_items = {};
  items_looked_at = {};
  published = {};
  published_field.clear();
  published_at = -1.0;
  sparks = 0;
  steam = 0;
  struck = false;
  own_burning.clear();
  looked_for_burning = false;
}

// One part's place this frame; false while there is none to report (not measured yet).
bool measure(std::size_t index, const std::uint8_t* attachment, void* thing, void* body,
             const float station_position[3], const float station_rotation[4], void* carried_by,
             double seconds, wire::StationProp& prop) {
  float thing_position[3];
  float thing_rotation[4];
  entity::world_pose(thing, thing_position, thing_rotation);
  std::optional<Measured>& part = measured[index];
  // The workpiece is always placed against the station: at the tongs' tip a hand's small
  // difference between the owner's body and its replica would put it a hand's breadth off the
  // anvil or the forge, and the anvil and the forge are what the eye measures it against.
  const bool workpiece = kParts[index].id == wire::StationPropId::Workpiece;
  bool hand_held = !workpiece && read<bool>(attachment, kAttachmentHangs);
  const std::int32_t slot = read<std::int32_t>(attachment, kAttachmentSlot);
  wire::StationAnchor anchor =
      (slot & 1) != 0 ? wire::StationAnchor::LeftHand : wire::StationAnchor::RightHand;
  // While the tongs carry the workpiece between the forge and the anvil it hangs from the tongs'
  // hand, so the replica's copy moves with the tongs: placed against the station it eased after
  // the owner's ten places a second and floated apart from them. Carried: while the
  // game has it attached (a guess by its speed changed over a moment late, and the piece jumped
  // between the tongs and the station on the other screens).
  if (workpiece) {
    const bool carried = read<bool>(attachment, kAttachmentHangs);
    // Taken relative to the tongs, as the game hangs it on them; a piece the game gives the
    // smith's hand without tongs (a blade, live: the tongs stay on their rack) hangs from that
    // hand (C_BlacksmithingWorkpieceAttachment: the hand slot's item, else the hand).
    float holder_position[3];
    float holder_rotation[4];
    wire::StationAnchor holder_anchor = wire::StationAnchor::Tongs;
    bool holder_found = false;
    if (carried_by && read<bool>(attachment, kWorkpieceOnTongs)) {
      entity::world_pose(carried_by, holder_position, holder_rotation);
      holder_found = true;
    } else {
      holder_anchor = anchor;
      holder_found = entity::joint_pose(body, joint_of(anchor), holder_position, holder_rotation);
    }
    if (carried && holder_found && distance(holder_position, thing_position) <= kTongsReach) {
      Measured now;
      now.anchor = holder_anchor;
      pose_math::relative(holder_position, holder_rotation, thing_position, thing_rotation,
                          now.position.data(), now.rotation.data());
      if (!part || part->anchor != holder_anchor) {
        log_line("SMITHING own W carried %s (%.2f m from them)",
                 holder_anchor == wire::StationAnchor::Tongs ? "in the tongs" : "in the hand",
                 distance(holder_position, thing_position));
        now.samples = 1;
        now.least_motion = 1e9f;
        now.last_position = now.position;
        now.last_rotation = now.rotation;
        std::memcpy(now.last_joint, holder_position, sizeof(holder_position));
        part = now;
      } else {
        // The piece hangs rigidly from its holder as the game attached it, but follows it a frame
        // late while the smith moves: an offset taken in every frame jumped about, and one kept
        // from the first frames could be taken while it trailed (the sword turned the
        // other way on other screens). The offset is taken again whenever it is the same as in
        // the frame before, which only the held offset is.
        const float moved = distance(now.position.data(), part->last_position.data());
        const float dot = std::fabs(
            now.rotation[0] * part->last_rotation[0] + now.rotation[1] * part->last_rotation[1] +
            now.rotation[2] * part->last_rotation[2] + now.rotation[3] * part->last_rotation[3]);
        if (moved < kOffsetSteady && 1.0f - dot < kTurnSteady) {
          part->position = now.position;
          part->rotation = now.rotation;
          part->settled = true;
        }
        part->last_position = now.position;
        part->last_rotation = now.rotation;
        std::memcpy(part->last_joint, holder_position, sizeof(holder_position));
      }
      prop.id = kParts[index].id;
      if (rests_on_station(station_position, station_rotation, thing_position, thing_rotation,
                           seconds, prop)) {
        return true;
      }
      prop.anchor = part->anchor;
      prop.position = part->position;
      prop.rotation = part->rotation;
      pose_math::normalize(prop.rotation.data());
      return true;
    }
    // Laid down: placed against the station anyway, and at rest from scratch when taken up again.
    workpiece_rest = {};
  }
  float joint_position[3];
  float joint_rotation[4];
  if (hand_held && !entity::joint_pose(body, joint_of(anchor), joint_position, joint_rotation)) {
    return false;
  }
  // A tool the minigame counts as held may lie elsewhere meanwhile (the tongs keep the iron in
  // the forge while both hands work the bellows, live): only one at the hand hangs from it.
  const float reach = hand_held ? distance(joint_position, thing_position) : -1.0f;
  if (hand_held && reach > kGripReach) {
    hand_held = false;
  }
  static std::array<int, kPartCount> logged_held{};
  const int held = hand_held ? 1 : (reach >= 0.0f ? 2 : 3);
  if (logged_held[index] != held) {
    logged_held[index] = held;
    log_line("SMITHING own %c %s (%.2f m from the %s) %u %s at %.2f,%.2f,%.2f",
             char(kParts[index].id),
             held == 1 ? "in the hand" : (held == 2 ? "away from the hand" : "not held"), reach,
             joint_of(anchor), entity::id_of(thing), entity::name_of(thing, 48).c_str(),
             thing_position[0], thing_position[1], thing_position[2]);
  }
  if (hand_held) {
    Measured now;
    now.anchor = anchor;
    pose_math::relative(joint_position, joint_rotation, thing_position, thing_rotation,
                        now.position.data(), now.rotation.data());
    if (!part || part->anchor != anchor) {
      now.samples = 1;
      now.least_motion = 1e9f;
      std::memcpy(now.last_joint, joint_position, sizeof(joint_position));
      part = now;
    } else if (!part->settled) {
      const float motion = distance(joint_position, part->last_joint);
      if (motion < part->least_motion) {
        part->least_motion = motion;
        part->position = now.position;
        part->rotation = now.rotation;
      }
      std::memcpy(part->last_joint, joint_position, sizeof(joint_position));
      part->settled = ++part->samples >= kSteadyFrames;
    } else {
      // The minigame turns a tool in the hand as it goes on (the tongs grip the iron at the anvil
      // otherwise than at the forge, live): whenever the hand is still the grip is taken again.
      const float motion = distance(joint_position, part->last_joint);
      if (motion < kHandStill) {
        part->position = now.position;
        part->rotation = now.rotation;
      }
      std::memcpy(part->last_joint, joint_position, sizeof(joint_position));
    }
    // Published from the first frame in the hand with the steadiest offset so far: held back
    // until it settled, the other screens kept the tool at the station meanwhile and the piece
    // hanging from it floated there while the smith walked away.
  } else {
    Measured now;
    now.anchor = wire::StationAnchor::Station;
    pose_math::relative(station_position, station_rotation, thing_position, thing_rotation,
                        now.position.data(), now.rotation.data());
    now.settled = true;
    part = now;
  }
  prop.id = kParts[index].id;
  prop.anchor = part->anchor;
  prop.position = part->position;
  prop.rotation = part->rotation;
  pose_math::normalize(prop.rotation.data());
  return true;
}

// ---------------------------------------------------------------------------------------------
// Every other client's side.

// A copy of a station's thing that a replica's action shows.
struct Copy {
  std::string name;
  std::string model;
  wire::StationAnchor anchor = wire::StationAnchor::Station;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  bool placed = false;
  int waited = 0;
  std::string glow;      // the heat zones' glow it shows
  double glow_at = 0.0;  // when it was last applied
  // One heat zone object per zone for the minigame's glow (zone_glow): its index at + 4, its heat
  // at + 8, a failure flag at + 24 and the part's material (a counted reference) at + 32.
  std::vector<std::array<std::uint8_t, 64>> glow_zones;
  // Where a copy lying at the station is shown now, easing towards the reported place.
  std::array<float, 3> shown_position{};
  std::array<float, 4> shown_rotation{0.0f, 0.0f, 0.0f, 1.0f};
  double eased_at = 0.0;
  // The entity whose attachment (`bound_slot`) holds this copy as the game's binding, 0 for none.
  std::uint32_t bound_to = 0;
  const char* bound_slot = nullptr;
  // The body whose hand slot (`hand_slot`, -1 none) holds this copy as the game holds it.
  std::uint32_t hand_body = 0;
  int hand_slot = -1;
};

// This frame's time (keep's `now`).
double frame_now = 0.0;

// A thing of the level's station that a replica's action hides or moves.
struct Real {
  std::uint32_t id = 0;
  float position[3] = {};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool changed = false;
  double looked_at = 0.0;  // when it was last looked for
};

// One of the level's effects at the station: whether it burned before the replica's action
// switched it.
struct Effect {
  std::uint32_t id = 0;
  bool burned = false;
  float strength = -1.0f;  // the strength it was given, -1 none
  // Its place in the level, kept while the replica's action moves it (overheated iron).
  bool moved = false;
  float position[3] = {};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

// The station's effects as a replica's action shows them.
struct Effects {
  bool found = false;
  Effect flare;
  Effect heat;
  Effect sparks;
  Effect steam;
  std::vector<Effect> burning;
  // The sparks' place in the level, before the strokes moved them.
  float sparks_position[3] = {};
  float sparks_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool sparks_moved = false;
  std::uint32_t light = 0;
  float light_colour[4] = {};
  bool light_saved = false;
  std::uint8_t forge = static_cast<std::uint8_t>(kLightSteps);
  // The bursts already shown (the first field's counts are only taken).
  bool counted = false;
  std::uint8_t strokes = 0;
  std::uint8_t quenches = 0;
  std::string sounds;                                     // the sounds' counts already played
  std::array<bool, wire::kStationSoundCount> sounding{};  // started on the body
  // Playing on the smith's own screen (started more often than stopped there), heard or not here.
  std::array<bool, wire::kStationSoundCount> wanted{};
  // Whether the local player is within hearing of the smith (kHearingReach).
  bool heard = false;
};

struct Shown {
  void* body = nullptr;
  std::uint32_t body_id = 0;
  std::string field;
  wire::StationAction action;
  std::uint32_t station = 0;
  float station_position[3] = {};
  float station_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool looked_for_tools = false;
  Real hammer;
  Real tongs;
  Real bellows;
  // The clip this client started on the bellows (-1 none), and when the pull was last logged.
  std::string bellows_clip;
  std::int32_t bellows_id = -1;  // the example clip the bag plays, -1 none
  bool bellows_loop = false;     // that clip is a pumping loop
  bool bellows_example = false;  // that clip is a blend space's example
  std::optional<Copy> hammer_copy;
  std::optional<Copy> tongs_copy;
  std::optional<Copy> workpiece_copy;
  Effects effects;
};

std::unordered_map<std::uint32_t, Shown> shown;
// Every spawn has a new name: the entity system removes an entity at the end of the frame, so a
// copy spawned under the name of one removed in the same frame would be found and lost with it.
unsigned generation = 0;

// The nearest entity of a class within `reach` of a place whose name starts with `prefix` (any
// for none).
void* nearest(const char* entity_class, const char* prefix, const float place[3], float reach) {
  void* best = nullptr;
  float best_distance = reach;
  entity::each_of_class(entity_class, [&](void* candidate) {
    if (prefix) {
      const std::string name = entity::name_of(candidate, 96);
      if (name.rfind(prefix, 0) != 0) {
        return;
      }
    }
    float there[3];
    entity::world_position(candidate, there);
    const float away = distance(there, place);
    if (away <= best_distance) {
      best = candidate;
      best_distance = away;
    }
  });
  return best;
}

void remember(Real& real, void* thing) {
  real = Real{};
  if (thing) {
    real.id = entity::id_of(thing);
    entity::world_pose(thing, real.position, real.rotation);
  }
}

// The level's thing back as the level had it.
void restore(Real& real, bool hidden_copy) {
  void* thing = real.changed ? entity::by_id(real.id) : nullptr;
  if (thing) {
    if (hidden_copy) {
      entity::hide(thing, false);
    } else {
      entity::place_rotated(thing, real.position, real.rotation);
    }
  }
  real.changed = false;
}

// The zone objects' references to their parts' materials (CMatInfo slot 3, Release).
constexpr std::size_t kZoneMaterial = 32;
constexpr std::size_t kMaterialRelease = 3;

void release_glow_zones(Copy& copy) {
  for (auto& zone : copy.glow_zones) {
    void* material = read<void*>(zone.data(), kZoneMaterial);
    if (material) {
      slot_of<void(__fastcall*)(void*)>(material, kMaterialRelease)(material);
    }
  }
  copy.glow_zones.clear();
}

// The tongs' attachment the game hangs the workpiece on (C_BlacksmithingWorkpieceAttachment,
// binds it, clears it): the tongs' character, its attachment manager
// (ICharacterInstance slot 9), the attachment by name (slot 8), a binding added to it (slot 26)
// or cleared (slot 30). The binding is a CEntityAttachment of the game's allocator: the
// attachment releases it.
constexpr char kTongsAttachment[] = "Assetslot";
constexpr std::size_t kAttachmentManager = 9;
constexpr std::size_t kAttachmentByName = 8;
constexpr std::size_t kAddBinding = 26;
constexpr std::size_t kClearBinding = 30;
constexpr std::size_t kEntityAttachmentSize = 40;
// CEntity::GetId.
constexpr std::size_t kEntityId = 1;

std::uint32_t id_of(void* thing) {
  using GetId = std::uint32_t(__fastcall*)(void* entity);
  return thing ? slot_of<GetId>(thing, kEntityId)(thing) : 0;
}

// An attachment of an entity's character by name, or null.
void* attachment_of(void* holder, const char* name) {
  void* character = character_of(holder);
  if (!character) {
    return nullptr;
  }
  void* manager = slot_of<Get>(character, kAttachmentManager)(character);
  if (!manager) {
    return nullptr;
  }
  using ByName = void*(__fastcall*)(void* manager, const char* name);
  return slot_of<ByName>(manager, kAttachmentByName)(manager, name);
}

// Hangs `piece` on the holder's attachment as the game hangs its workpiece (the tongs'
// "Assetslot", or a hand's item slot); false when the holder has no such attachment.
bool bind_to(void* holder, const char* name, void* piece) {
  void* slot = attachment_of(holder, name);
  if (!slot || !piece || !natives.allocate || !natives.entity_attachment) {
    return false;
  }
  using Allocate = void*(__fastcall*)(std::size_t size);
  void* memory = reinterpret_cast<Allocate>(natives.allocate)(kEntityAttachmentSize);
  if (!memory) {
    return false;
  }
  using Construct = void*(__fastcall*)(void* memory, void* bound, void* holder, char flags);
  void* binding = reinterpret_cast<Construct>(natives.entity_attachment)(memory, piece, holder, 0);
  using AddBinding =
      std::uint32_t(__fastcall*)(void* slot, void* binding, void* skin, std::uint32_t flags);
  slot_of<AddBinding>(slot, kAddBinding)(slot, binding, nullptr, 0);
  return true;
}

void* hands_of(void* body) {
  void* actor = body && natives.actor_of ? natives.actor_of(id_of(body)) : nullptr;
  return actor ? slot_of<Get>(actor, kHandSlots)(actor) : nullptr;
}

// Puts `thing` in the body's hand slot as the game puts a smith's tool or iron there; false when
// the body is no actor.
bool hand_take(void* body, int slot, void* thing) {
  void* hands = hands_of(body);
  if (!hands || !thing || !natives.hand_take) {
    return false;
  }
  using Take = void(__fastcall*)(void* hands, std::uint32_t entity, int slot, const char* name,
                                 const char* joint, bool flag);
  reinterpret_cast<Take>(natives.hand_take)(hands, id_of(thing), slot,
                                            slot == 0 ? kRightEntity : kLeftEntity, nullptr, true);
  return true;
}

void hand_release(Copy& copy) {
  if (copy.hand_slot < 0) {
    return;
  }
  void* hands = hands_of(entity::by_id(copy.hand_body));
  if (hands && natives.hand_release) {
    using Release =
        void(__fastcall*)(void* hands, int slot, const char* name, bool keep, bool flag);
    reinterpret_cast<Release>(natives.hand_release)(
        hands, copy.hand_slot, copy.hand_slot == 0 ? kRightEntity : kLeftEntity, false, true);
  }
  copy.hand_slot = -1;
  copy.hand_body = 0;
}

void unbind(Copy& copy) {
  hand_release(copy);
  if (copy.bound_to == 0) {
    return;
  }
  if (void* slot = attachment_of(entity::by_id(copy.bound_to), copy.bound_slot)) {
    using ClearBinding = void(__fastcall*)(void* slot, std::uint32_t flags);
    slot_of<ClearBinding>(slot, kClearBinding)(slot, 0);
  }
  copy.bound_to = 0;
}

void drop(std::optional<Copy>& copy) {
  if (copy) {
    unbind(*copy);
    release_glow_zones(*copy);
    entity::remove(copy->name);
    copy.reset();
  }
}

// An effect of the level as it burned before.
void restore(const Effect& effect) {
  void* thing = entity::by_id(effect.id);
  if (thing && burns(thing) != effect.burned) {
    switch_effect(thing, effect.burned);
  }
  if (thing && effect.moved) {
    entity::place_rotated(thing, effect.position, effect.rotation);
  }
}

void restore(Effects& effects) {
  if (!effects.found) {
    return;
  }
  restore(effects.flare);
  restore(effects.heat);
  for (const auto& burning : effects.burning) {
    restore(burning);
  }
  void* anvil_sparks = effects.sparks_moved ? entity::by_id(effects.sparks.id) : nullptr;
  if (anvil_sparks) {
    entity::place_rotated(anvil_sparks, effects.sparks_position, effects.sparks_rotation);
  }
  float* colour = effects.light_saved ? light_colour(entity::by_id(effects.light)) : nullptr;
  if (colour) {
    std::memcpy(colour, effects.light_colour, sizeof(effects.light_colour));
  }
  effects = Effects{};
}

// The station's bellows back at rest: the clip this client started on them stops.
void stop_bellows(Shown& entry) {
  if (entry.bellows_clip.empty()) {
    return;
  }
  entry.bellows_clip.clear();
  entry.bellows_id = -1;
  // After the owner's own clip (the lever put back) the bag keeps that clip's last pose; only a
  // pumping example is stopped.
  if (!entry.bellows_example) {
    return;
  }
  void* skeleton = skeleton_of(entity::by_id(entry.bellows.id));
  if (skeleton && natives.stop_clips) {
    natives.stop_clips(skeleton);
  }
}

void release(Shown& entry) {
  drop(entry.hammer_copy);
  drop(entry.tongs_copy);
  drop(entry.workpiece_copy);
  restore(entry.hammer, true);
  restore(entry.tongs, true);
  stop_bellows(entry);
  // A loop the replica's body still plays (the forge's roar, the bellows) ends with the action;
  // the body is looked up again, as it may be gone already.
  void* body = entity::by_id(entry.body_id);
  for (std::size_t i = 0; body && i < wire::kStationSoundCount; ++i) {
    if (entry.effects.sounding[i]) {
      sound_on(body, i, false);
    }
  }
  restore(entry.effects);
}

Effect effect_near(const char* entity_class, const char* prefix, const float place[3]) {
  Effect effect;
  void* thing = nearest(entity_class, prefix, place, kToolReach);
  if (thing) {
    effect.id = entity::id_of(thing);
    effect.burned = burns(thing);
  }
  return effect;
}

// The level's effects at the replica's station, as they burn before its action.
void find_effects(std::uint32_t holder, Shown& entry) {
  Effects& effects = entry.effects;
  effects = Effects{};
  effects.found = true;
  const float* place = entry.station_position;
  effects.flare = effect_near(kParticleClass, kFlarePrefix, place);
  effects.heat = effect_near(kParticleClass, kHeatPrefix, place);
  effects.sparks = effect_near(kParticleClass, kSparksPrefix, place);
  effects.steam = effect_near(kParticleClass, kSteamPrefix, place);
  std::vector<std::uint32_t> burning = all_near(kParticleClass, kBurningPrefix, place, kToolReach);
  by_place(burning);
  for (const std::uint32_t id : burning) {
    effects.burning.push_back(Effect{id, burns(entity::by_id(id))});
  }
  if (void* anvil_sparks = entity::by_id(effects.sparks.id)) {
    entity::world_pose(anvil_sparks, effects.sparks_position, effects.sparks_rotation);
  }
  void* light = nearest(kLightClass, kLightPrefix, place, kToolReach);
  const float* colour = light_colour(light);
  if (colour) {
    effects.light = entity::id_of(light);
    std::memcpy(effects.light_colour, colour, sizeof(effects.light_colour));
    effects.light_saved = true;
  }
  log_line("SMITHING holder=%u effects flare %u heat %u sparks %u steam %u smoke %zu light %u",
           holder, effects.flare.id, effects.heat.id, effects.sparks.id, effects.steam.id,
           effects.burning.size(), effects.light);
}

// A place relative to the station, in the world.
void station_to_world(const Shown& entry, const std::array<float, 3>& position,
                      const std::array<float, 4>& rotation, float world_position[3],
                      float world_rotation[4]) {
  pose_math::compose(entry.station_position, entry.station_rotation, position.data(),
                     rotation.data(), world_position, world_rotation);
}

void keep_effect(const Effect& effect, bool on) {
  void* thing = entity::by_id(effect.id);
  if (thing && burns(thing) != on) {
    switch_effect(thing, on);
  }
}

// The anvil's ring among the minigame's sounds (wire::kStationSounds).
constexpr std::size_t kAnvilHit = 7;
// How far from the smith another player hears the minigame. The game made its sounds for the
// player at the anvil and does not fade them with distance (a smith heard across
// the map), so farther than this a watcher hears none; a little farther still before the
// sounds stop, so standing at the edge does not switch them on and off.
constexpr float kHearingReach = 30.0f;
constexpr float kHearingSlack = 5.0f;
// The loops of the minigame (the forge's roar, the overheated iron): started again for a watcher
// who comes into hearing while the smith works. The others are single strokes and moves.
constexpr std::size_t kLoopSounds[] = {0, 5};

// Whether the local player is within `reach` metres of the smith's body.
bool within(void* body, float reach) {
  if (!body || !local_body) {
    return false;
  }
  float here[3];
  float there[3];
  entity::world_position(local_body, here);
  entity::world_position(body, there);
  return distance(here, there) <= reach;
}

// Starts or stops one of the smith's sounds as the smith's own game did, heard only within reach.
void hear(Shown& entry, std::size_t sound, bool start) {
  Effects& effects = entry.effects;
  effects.wanted[sound] = start;
  if (start && effects.heard) {
    sound_on(entry.body, sound, true);
    effects.sounding[sound] = true;
  } else if (!start && effects.sounding[sound]) {
    sound_on(entry.body, sound, false);
    effects.sounding[sound] = false;
  }
}

// The local player came into or went out of hearing: the loops start again or everything stops.
void update_hearing(std::uint32_t holder, Shown& entry) {
  Effects& effects = entry.effects;
  const bool heard =
      within(entry.body, effects.heard ? kHearingReach + kHearingSlack : kHearingReach);
  if (heard == effects.heard) {
    return;
  }
  effects.heard = heard;
  log_line("SMITHING holder=%u %s hearing", holder, heard ? "in" : "out of");
  if (!heard) {
    for (std::size_t i = 0; i < wire::kStationSoundCount; ++i) {
      if (effects.sounding[i]) {
        sound_on(entry.body, i, false);
        effects.sounding[i] = false;
      }
    }
    return;
  }
  for (const std::size_t loop : kLoopSounds) {
    if (effects.wanted[loop] && !effects.sounding[loop]) {
      sound_on(entry.body, loop, true);
      effects.sounding[loop] = true;
    }
  }
}

int hex_value(char digit) {
  return digit <= '9' ? digit - '0' : digit - 'a' + 10;
}

// The minigame's sounds on the replica's body: every start and stop the owner's game made since
// the last field. With the first field a sound started more often than stopped is started (the
// forge's roar, which the minigame starts as it begins, or a loop a late joiner comes into).
void keep_sounds(std::uint32_t holder, Shown& entry) {
  Effects& effects = entry.effects;
  const std::string& counts = entry.action.sounds;
  update_hearing(holder, entry);
  if (effects.sounds.empty()) {
    effects.sounds = counts;
    for (std::size_t i = 0; i < wire::kStationSoundCount; ++i) {
      if (counts[i] != counts[wire::kStationSoundCount + i]) {
        hear(entry, i, true);
        log_line("SMITHING holder=%u sound %s (playing)", holder, wire::kStationSounds[i]);
      }
    }
    return;
  }
  if (counts == effects.sounds) {
    return;
  }
  for (std::size_t i = 0; i < wire::kStationSoundCount; ++i) {
    const int starts = (hex_value(counts[i]) - hex_value(effects.sounds[i]) + 16) % 16;
    const std::size_t stop_at = wire::kStationSoundCount + i;
    const int stops = (hex_value(counts[stop_at]) - hex_value(effects.sounds[stop_at]) + 16) % 16;
    if (stops > 0) {
      hear(entry, i, false);
    }
    if (starts > 0) {
      hear(entry, i, true);
      log_line("SMITHING holder=%u sound %s%s", holder, wire::kStationSounds[i],
               effects.heard ? "" : " (out of hearing)");
    }
  }
  effects.sounds = counts;
}

// The overheated iron's burning effects on the replica's copy of the piece, every frame, where the
// owner's game put them against its piece (the field's O).
void place_burning(Shown& entry, void* piece) {
  const wire::StationAction& action = entry.action;
  if (!piece || action.burning_at.size() != 3 * action.burning.size() ||
      action.burning.size() != entry.effects.burning.size()) {
    return;
  }
  float piece_position[3];
  float piece_rotation[4];
  entity::world_pose(piece, piece_position, piece_rotation);
  for (std::size_t i = 0; i < entry.effects.burning.size(); ++i) {
    Effect& burning = entry.effects.burning[i];
    void* thing = entity::by_id(burning.id);
    if (!thing || action.burning[i] <= 0.0f) {
      continue;
    }
    if (!burning.moved) {
      entity::world_pose(thing, burning.position, burning.rotation);
      burning.moved = true;
    }
    const float offset[3] = {action.burning_at[3 * i], action.burning_at[3 * i + 1],
                             action.burning_at[3 * i + 2]};
    const float none[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float world[3];
    float turn[4];
    pose_math::compose(piece_position, piece_rotation, offset, none, world, turn);
    entity::place_rotated(thing, world, burning.rotation);
  }
}

// The station's effects as the replica's action shows them.
void keep_effects(std::uint32_t holder, Shown& entry) {
  Effects& effects = entry.effects;
  const wire::StationAction& action = entry.action;
  keep_effect(effects.flare, (action.effects & wire::kStationForgeFlare) != 0);
  keep_effect(effects.heat, (action.effects & wire::kStationForgeHeat) != 0);
  // Each overheated iron's smoke burns as strongly as the owner's (all alike when it told none).
  const bool told = action.burning.size() == effects.burning.size();
  for (std::size_t i = 0; i < effects.burning.size(); ++i) {
    Effect& burning = effects.burning[i];
    const bool any = (action.effects & wire::kStationBurning) != 0;
    const bool on = told ? action.burning[i] > 0.0f : any;
    keep_effect(burning, on);
    const float strength = told && on ? action.burning[i] : -1.0f;
    if (strength >= 0.0f && std::fabs(strength - burning.strength) > kBurningStep / 2.0f) {
      emitters::Given given;
      given.strength = strength;
      emitters::give(entity::by_id(burning.id), given);
      log_line("SMITHING holder=%u burning %zu at %.2f", holder, i, strength);
    }
    burning.strength = strength;
  }
  if (effects.light_saved && action.forge != effects.forge) {
    float* colour = light_colour(entity::by_id(effects.light));
    if (colour) {
      const float scale = static_cast<float>(action.forge) / kLightSteps;
      for (int channel = 0; channel < 3; ++channel) {
        colour[channel] = effects.light_colour[channel] * scale;
      }
      effects.forge = action.forge;
    }
  }
  keep_sounds(holder, entry);
  if (!effects.counted) {
    effects.counted = true;
  } else {
    if (action.strokes != effects.strokes) {
      // The sparks fly where the owner's hammer struck, as many as it threw.
      void* anvil_sparks = entity::by_id(effects.sparks.id);
      if (anvil_sparks && action.sparks_at) {
        float position[3];
        float rotation[4];
        const std::array<float, 3> at{action.sparks[0], action.sparks[1], action.sparks[2]};
        const std::array<float, 4> none{0.0f, 0.0f, 0.0f, 1.0f};
        station_to_world(entry, at, none, position, rotation);
        entity::place_rotated(anvil_sparks, position, effects.sparks_rotation);
        effects.sparks_moved = true;
      }
      // Set off as the game's stroke does it: only switched on, a burst kept the emitter's last
      // count (no big burst on a perfect stroke on the other screens).
      restart_effect(anvil_sparks, action.sparks[3]);
      // The hammer's ring on the anvil comes with every burst of sparks (the owner's game plays
      // it where the hammer strikes, not through the body's sounds).
      if (effects.heard) {
        sound_on(entry.body, kAnvilHit, true);
      }
      log_line("SMITHING holder=%u sparks %u at %.2f,%.2f,%.2f x%.2f", holder,
               unsigned(action.strokes), action.sparks[0], action.sparks[1], action.sparks[2],
               action.sparks[3]);
    }
    if (action.quenches != effects.quenches) {
      switch_effect(entity::by_id(effects.steam.id), true);
      log_line("SMITHING holder=%u steam %u", holder, unsigned(action.quenches));
    }
  }
  effects.strokes = action.strokes;
  effects.quenches = action.quenches;
}

// Shows a copy of `model` (with `material`) at the prop's place: hanging from the body's hand or
// lying at the station.
// A child entity taken off its parent (CEntity::DetachThis, vtable slot 24; called
// on the child), its place then set again by the caller.
constexpr std::size_t kDetachThis = 24;

void detach(void* thing) {
  using DetachThis = void(__fastcall*)(void* entity, int flags, int attachment_flags);
  slot_of<DetachThis>(thing, kDetachThis)(thing, 0, 0);
}

void keep_copy(std::uint32_t holder, Shown& entry, std::optional<Copy>& copy,
               const std::string& model, const std::string& material,
               const wire::StationProp& prop) {
  // A copy that moves to another anchor (station, hand, tongs) stays the same entity: it is
  // detached and placed or attached again. Removed and spawned anew it was gone for a frame or
  // more every time the owner took the piece up, laid it down or turned it (the piece
  // blinked on other screens, 51 new copies in one session).
  if (copy && copy->model == model && copy->anchor != prop.anchor && copy->placed) {
    void* moved = entity::find(copy->name);
    if (moved) {
      if (copy->bound_to != 0 || copy->hand_slot >= 0) {
        unbind(*copy);
      } else if (copy->anchor != wire::StationAnchor::Station) {
        detach(moved);
      }
      copy->anchor = prop.anchor;
      copy->placed = false;
      copy->waited = 0;
    }
  }
  if (copy && (copy->model != model || copy->anchor != prop.anchor ||
               (copy->waited > kAttachFrames && !copy->placed))) {
    drop(copy);
  }
  if (!copy) {
    float at[3];
    entity::world_position(entry.body, at);
    char name[64];
    std::snprintf(name, sizeof(name), "kcds_station_prop_%u_%u", holder, ++generation);
    if (!entity::spawn_mesh(name, at, model, material)) {
      log_line("SMITHING holder=%u could not show %s", holder, model.c_str());
      return;
    }
    copy = Copy{name, model, prop.anchor, prop.position, prop.rotation, false, 0};
  }
  void* thing = entity::find(copy->name);
  if (!thing) {
    ++copy->waited;
    return;
  }
  const bool moved = copy->position != prop.position || copy->rotation != prop.rotation;
  copy->position = prop.position;
  copy->rotation = prop.rotation;
  if (prop.anchor == wire::StationAnchor::Station) {
    float position[3];
    float rotation[4];
    station_to_world(entry, prop.position, prop.rotation, position, rotation);
    if (!copy->placed) {
      std::memcpy(copy->shown_position.data(), position, sizeof(position));
      std::memcpy(copy->shown_rotation.data(), rotation, sizeof(rotation));
      entity::place_rotated(thing, position, rotation);
      copy->placed = true;
      copy->eased_at = frame_now;
      return;
    }
    // A place reported a tenth of a second apart is eased into, so the copy glides as the
    // owner's moves.
    if (distance(copy->shown_position.data(), position) > kSettled || moved) {
      const double since = frame_now - copy->eased_at;
      float share = static_cast<float>(since * kEaseRate);
      share = share < 0.0f ? 0.0f : (share > 1.0f ? 1.0f : share);
      for (int axis = 0; axis < 3; ++axis) {
        copy->shown_position[axis] += (position[axis] - copy->shown_position[axis]) * share;
      }
      const float dot =
          copy->shown_rotation[0] * rotation[0] + copy->shown_rotation[1] * rotation[1] +
          copy->shown_rotation[2] * rotation[2] + copy->shown_rotation[3] * rotation[3];
      const float sign = dot < 0.0f ? -1.0f : 1.0f;
      for (int axis = 0; axis < 4; ++axis) {
        copy->shown_rotation[axis] += (sign * rotation[axis] - copy->shown_rotation[axis]) * share;
      }
      pose_math::normalize(copy->shown_rotation.data());
      entity::place_rotated(thing, copy->shown_position.data(), copy->shown_rotation.data());
    }
    copy->eased_at = frame_now;
    return;
  }
  // Tongs gone (their copy made anew): hung again on the ones there now.
  if (copy->bound_to != 0 && !entity::by_id(copy->bound_to)) {
    copy->bound_to = 0;
    copy->placed = false;
  }
  if (!copy->placed) {
    if (!natives.attach_child) {
      return;
    }
    if (prop.anchor == wire::StationAnchor::Tongs) {
      // Carried in the tongs: a child of the tongs' copy (or the station's own tongs), so it moves
      // with them as the game's iron hangs on the tongs.
      void* tongs = entry.tongs_copy && entry.tongs_copy->placed
                        ? entity::find(entry.tongs_copy->name)
                        : entity::by_id(entry.tongs.id);
      if (!tongs) {
        ++copy->waited;
        return;
      }
      // Hung on the tongs' "Assetslot" as the game hangs its iron, so it sits in the tongs as on
      // the smith's screen; tongs without that attachment hold it as a child at the reported
      // place.
      copy->placed = true;
      if (bind_to(tongs, kTongsAttachment, thing)) {
        copy->bound_to = id_of(tongs);
        copy->bound_slot = kTongsAttachment;
        log_line("SMITHING holder=%u %s on the tongs' slot", holder, model.c_str());
        return;
      }
      const AttachParams params{0, nullptr};
      reinterpret_cast<AttachChild>(natives.attach_child)(tongs, thing, &params);
      log_line("SMITHING holder=%u %s in the tongs", holder, model.c_str());
    } else {
      // Held in the hand's item slot as the game holds the smith's tools and an iron without
      // tongs, so it sits in the hand as on the smith's screen (an offset measured on the owner's
      // side turned it about).
      copy->placed = true;
      const int slot = prop.anchor == wire::StationAnchor::LeftHand ? 1 : 0;
      if (hand_take(entry.body, slot, thing)) {
        copy->hand_body = id_of(entry.body);
        copy->hand_slot = slot;
        log_line("SMITHING holder=%u %s in hand slot %d", holder, model.c_str(), slot);
        return;
      }
      const AttachParams params{kAttachToJoint, joint_of(prop.anchor)};
      reinterpret_cast<AttachChild>(natives.attach_child)(entry.body, thing, &params);
      log_line("SMITHING holder=%u %s in the %s", holder, model.c_str(), joint_of(prop.anchor));
    }
  }
  // The game's binding places it from now on.
  if (copy->bound_to != 0 || copy->hand_slot >= 0) {
    return;
  }
  // Just attached: at the reported offset. Then eased towards each new one, as the copy lying at
  // the station is: the game moves the piece in the hand while it quenches and looks at it, and
  // offsets ten a second jerked it on other screens.
  if (copy->waited >= 0) {
    std::copy(prop.position.begin(), prop.position.end(), copy->shown_position.begin());
    std::copy(prop.rotation.begin(), prop.rotation.end(), copy->shown_rotation.begin());
    entity::place_rotated(thing, prop.position.data(), prop.rotation.data());
    copy->waited = -1;
    copy->eased_at = frame_now;
    return;
  }
  if (moved || distance(copy->shown_position.data(), prop.position.data()) > kSettled) {
    const double since = frame_now - copy->eased_at;
    float share = static_cast<float>(since * kEaseRate);
    share = share < 0.0f ? 0.0f : (share > 1.0f ? 1.0f : share);
    for (int axis = 0; axis < 3; ++axis) {
      copy->shown_position[axis] += (prop.position[axis] - copy->shown_position[axis]) * share;
    }
    const float dot =
        copy->shown_rotation[0] * prop.rotation[0] + copy->shown_rotation[1] * prop.rotation[1] +
        copy->shown_rotation[2] * prop.rotation[2] + copy->shown_rotation[3] * prop.rotation[3];
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    for (int axis = 0; axis < 4; ++axis) {
      copy->shown_rotation[axis] +=
          (sign * prop.rotation[axis] - copy->shown_rotation[axis]) * share;
    }
    pose_math::normalize(copy->shown_rotation.data());
    entity::place_rotated(thing, copy->shown_position.data(), copy->shown_rotation.data());
  }
  copy->eased_at = frame_now;
}

const wire::StationProp* prop_of(const wire::StationAction& action, wire::StationPropId id) {
  for (const auto& prop : action.props) {
    if (prop.id == id) {
      return &prop;
    }
  }
  return nullptr;
}

// A tool of the station: hidden while the replica's copy shows it, where the action puts it or
// where the level's own lies.
void keep_tool(std::uint32_t holder, Shown& entry, Real& real, std::optional<Copy>& copy,
               const char* item_class, const char* prefix, wire::StationPropId id) {
  void* thing = entity::by_id(real.id);
  if (!thing && frame_now - real.looked_at >= kLookAgain) {
    // A station's item slot fills when the level streams in around it, so a player who has just
    // come may find the tool a little later.
    remember(real, nearest(kItemClass, prefix, entry.station_position, kToolReach));
    real.looked_at = frame_now;
    thing = entity::by_id(real.id);
  }
  const wire::StationProp* reported = prop_of(entry.action, id);
  if (!thing && !reported) {
    drop(copy);
    return;
  }
  wire::StationProp prop;
  if (reported) {
    prop = *reported;
  } else {
    prop.id = id;
    prop.anchor = wire::StationAnchor::Station;
    pose_math::relative(entry.station_position, entry.station_rotation, real.position,
                        real.rotation, prop.position.data(), prop.rotation.data());
  }
  wire::Guid guid{};
  std::string model;
  std::string material;
  if (!wire::parse_guid(item_class, guid) || !animation_props::item_mesh(guid, model, material)) {
    return;
  }
  keep_copy(holder, entry, copy, model, material, prop);
  if (thing && copy && copy->placed && !real.changed) {
    entity::hide(thing, true);
    real.changed = true;
  }
}

// The bag's blend spaces and their example clips (Animations/assets/smithery/forge_bag/
// blendspace/*.bspace): the axe's by the pull alone (BlendWeight2), the sword's by the reach
// (BlendWeight: close 0, far 1) and the pull (idle 0, pumping 1).
struct BagBlendSpace {
  const char* name;
  const char* idle_close;
  const char* idle_far;
  const char* loop_close;
  const char* loop_far;
};
constexpr BagBlendSpace kBagBlendSpaces[] = {
    {"1d-blacksmith_axe_heating", "blacksmithing_axe_heating_idle",
     "blacksmithing_axe_heating_idle", "blacksmithing_axe_heating_loop",
     "blacksmithing_axe_heating_loop"},
    {"2d-blacksmith_sword_heating", "blacksmithing_sword_heating_idle_close",
     "blacksmithing_sword_heating_idle_far", "blacksmithing_sword_heating_loop_close",
     "blacksmithing_sword_heating_loop_far"},
};
// The normalized time of the newest clip on a body's layer 0, 0 for none.
float body_clip_time(void* body) {
  return clips::top_clip_time(body);
}

// The station's bellows play what the owner's bellows play, pulled as the replica's hands pull: the
// owner's game sets the same motion parameters on the smith and on the bag (smithing.hpp).
void keep_bellows(std::uint32_t holder, Shown& entry, const BodyMotion& motion) {
  void* character = character_of(entity::by_id(entry.bellows.id));
  void* skeleton = skeleton_of(entity::by_id(entry.bellows.id));
  if (!skeleton || !natives.start_clip) {
    return;
  }
  const std::string& clip = entry.action.bellows_clip;
  if (clip.empty()) {
    stop_bellows(entry);
    return;
  }
  // What the bag plays now: a blend space's nearest example, or the clip itself.
  std::string shown = clip;
  bool example = false;
  for (const BagBlendSpace& space : kBagBlendSpaces) {
    if (clip != space.name) {
      continue;
    }
    const bool pumping = (motion.mask & (1u << kPullParameter)) != 0 &&
                         std::isfinite(motion.values[kPullParameter]) &&
                         motion.values[kPullParameter] >= 0.5f;
    const bool reaching = (motion.mask & (1u << kReachParameter)) != 0 &&
                          std::isfinite(motion.values[kReachParameter]) &&
                          motion.values[kReachParameter] >= 0.5f;
    shown = pumping ? (reaching ? space.loop_far : space.loop_close)
                    : (reaching ? space.idle_far : space.idle_close);
    example = true;
  }
  if (shown == entry.bellows_clip) {
    // The game does not run the bag's clip on another client (its time stands still, live), but
    // a clip's start puts the bag in that clip's pose: while the bag pumps, its loop starts again
    // every frame at the body's blend-space time, so the lever follows the replica's hands. The
    // owner's own clips (taking the lever, putting it back) are driven so by the body's clip too:
    // started once they stood at their first frame (the lever stayed at rest while
    // the smith reached for it, and stayed out after Q was let go).
    if (entry.bellows_id >= 0 && natives.clear_clips) {
      const std::uint32_t flags = example ? kLoopFlag : (entry.action.bellows_flags & ~kLoopFlag);
      const float time = body_clip_time(entry.body);
      natives.clear_clips(skeleton);
      natives.start_clip(skeleton, entry.bellows_id, flags | kStartAtKeyTime, 0.0f, time);
    }
    return;
  }
  const std::int32_t id = clip_id(character, shown);
  bool started = false;
  if (id >= 0) {
    using PlaybackScale = void(__fastcall*)(void* skeleton, unsigned layer, float scale);
    slot_of<PlaybackScale>(skeleton, kLayerPlaybackScale)(skeleton, 0, 1.0f);
    // An example loops in step with the body's own blend space; the clip itself as the owner's.
    const std::uint32_t flags =
        example ? (kLoopFlag | kStartAtKeyTime) : entry.action.bellows_flags;
    const float key_time = example ? body_clip_time(entry.body) : 0.0f;
    // Without a blend: the bag's time stands still here, so a blend in never ended and the lever
    // stayed at rest until the first pull.
    started = natives.start_clip(skeleton, id, flags, 0.0f, key_time);
  }
  entry.bellows_clip = shown;
  entry.bellows_id = started ? id : -1;
  entry.bellows_example = example;
  entry.bellows_loop = shown.find("_loop") != std::string::npos;
  log_line("SMITHING holder=%u bellows %s plays %s (%d) %s", holder, clip.c_str(), shown.c_str(),
           id, started ? "started" : "refused");
}

// Finds the replica's station among this client's own and the things lying at it.
bool find_station(std::uint32_t holder, Shown& entry) {
  if (entry.station != 0 && entity::by_id(entry.station)) {
    return true;
  }
  void* station = nearest(kStationClass, nullptr, entry.action.station.data(), kStationMatch);
  if (!station) {
    return false;
  }
  entry.station = entity::id_of(station);
  entity::world_pose(station, entry.station_position, entry.station_rotation);
  if (!entry.looked_for_tools) {
    entry.looked_for_tools = true;
    remember(entry.hammer, nearest(kItemClass, kHammerPrefix, entry.station_position, kToolReach));
    remember(entry.tongs, nearest(kItemClass, kTongsPrefix, entry.station_position, kToolReach));
    remember(entry.bellows,
             nearest(kMeshClass, kBellowsPrefix, entry.station_position, kToolReach));
    log_line("SMITHING holder=%u station %u hammer %u tongs %u bellows %u", holder, entry.station,
             entry.hammer.id, entry.tongs.id, entry.bellows.id);
    find_effects(holder, entry);
  }
  return true;
}

using CanUse = bool(__fastcall*)(std::uint32_t station, std::uint32_t user);
CanUse original_can_use = nullptr;
using CloseRecipeBook = void(__fastcall*)(void* capture);
CloseRecipeBook close_recipe_book = nullptr;
std::uint32_t refused_station = 0;

bool __fastcall hooked_can_use(std::uint32_t station, std::uint32_t user) {
  if (!original_can_use(station, user)) {
    return false;
  }
  void* thing = entity::by_id(station);
  float where[3];
  if (!thing) {
    return true;
  }
  entity::world_position(thing, where);
  if (!station_taken(where)) {
    refused_station = 0;
    return true;
  }
  if (refused_station != station) {
    refused_station = station;
    log_line("SMITHING station %u is taken by another player", station);
  }
  return false;
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  const std::size_t at =
      kcd2s::bridge::rtti_vtable(natives.image, natives.image_size,
                                 reinterpret_cast<std::uintptr_t>(natives.image), kMinigameType);
  if (at == kcd2s::bridge::knpos) {
    log_line("SMITHING the minigame's vtable is missing");
    return false;
  }
  void* target = read<void*>(natives.image, at + kUpdateSlot * sizeof(void*));
  if (!target ||
      MH_CreateHook(target, reinterpret_cast<void*>(&hooked_update),
                    reinterpret_cast<void**>(&original_update)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    log_line("SMITHING could not hook the minigame's update %p", target);
    return false;
  }
  log_line("SMITHING minigame update %p hooked", target);
  // The particle switch: the bursts the owner's game sets off are counted; without it the
  // station's effects stay as the level has them on every other client.
  if (natives.particle_enable &&
      (MH_CreateHook(natives.particle_enable, reinterpret_cast<void*>(&hooked_particle_enable),
                     reinterpret_cast<void**>(&original_particle_enable)) != MH_OK ||
       MH_EnableHook(natives.particle_enable) != MH_OK)) {
    log_line("SMITHING could not hook the particle switch %p", natives.particle_enable);
    original_particle_enable = nullptr;
  }
  close_recipe_book = reinterpret_cast<CloseRecipeBook>(natives.close_recipe_book);
  // One smith at a station: the game's own check says no while another player smiths there.
  if (natives.can_use && (MH_CreateHook(natives.can_use, reinterpret_cast<void*>(&hooked_can_use),
                                        reinterpret_cast<void**>(&original_can_use)) != MH_OK ||
                          MH_EnableHook(natives.can_use) != MH_OK)) {
    log_line("SMITHING could not hook the station's check %p", natives.can_use);
  }
  return true;
}

std::string own_field(void* actor, void* body, double now) {
  local_actor = actor;
  local_body = body;
  if (body) {
    count_sounds(body);
  }
  if (!actor || !body || !minigame || GetTickCount64() - minigame_seen > kUpdateGraceMs ||
      read<void*>(minigame, kActor) != actor || !read<bool>(minigame, kRunning)) {
    if (!published_field.empty()) {
      log_line("SMITHING own action ended");
      // The next session counts its sounds afresh (those of its start come before its first
      // update, so they are kept until a session ends).
      sound_starts = {};
      sound_stops = {};
    }
    chosen_recipe.clear();
    last_state = 0;
    minigame = nullptr;
    clear_owner();
    return {};
  }
  void* station = entity::by_id(read<std::uint32_t>(minigame, kStation));
  auto* holder = static_cast<std::uint8_t*>(minigame) + kContextHolder;
  auto* context = static_cast<std::uint8_t*>(
      reinterpret_cast<Get>((*reinterpret_cast<void***>(holder))[kContextSlot])(holder));
  if (!station || !context) {
    return published_field;
  }
  float station_position[3];
  float station_rotation[4];
  entity::world_pose(station, station_position, station_rotation);
  const std::string recipe = recipe_id_of(minigame);
  if (!recipe.empty() && recipe != chosen_recipe) {
    chosen_recipe = recipe;
    recipe_to_report = recipe;
    std::memcpy(recipe_station.data(), station_position, sizeof(station_position));
    log_line("SMITHING own recipe %s", recipe.c_str());
  }
  silence_barks(minigame);
  using GetState = std::uint8_t(__fastcall*)(void* property);
  void* state_property = context + kStateProperty;
  const std::uint8_t state = slot_of<GetState>(state_property, kStateGetter)(state_property);
  if (state != last_state) {
    log_line("SMITHING own state %u", unsigned(state));
    if (state == kBrokenWorkpiece) {
      broken_to_report = true;
      std::memcpy(recipe_station.data(), station_position, sizeof(station_position));
      log_line("SMITHING own workpiece broke (recipe %s)", chosen_recipe.c_str());
    }
    last_state = state;
  }
  wire::StationAction action;
  action.kind = wire::kStationKindSmith;
  std::memcpy(action.station.data(), station_position, sizeof(station_position));
  for (std::size_t i = 0; i < kPartCount; ++i) {
    const auto* attachment = read<const std::uint8_t*>(context, kParts[i].offset);
    void* slot =
        attachment ? entity::by_id(read<std::uint32_t>(attachment, kAttachmentEntity)) : nullptr;
    void* thing = slot ? moved_by(i, slot) : nullptr;
    if (!thing) {
      measured[i].reset();
      continue;
    }
    if (kParts[i].id == wire::StationPropId::Bellows) {
      // The bellows stay where they stand; their lever moves by their own clip (smithing.hpp).
      const std::int32_t clip = top_clip(skeleton_of(thing), action.bellows_flags);
      action.bellows_clip = clip_name(character_of(thing), clip);
      static std::string refused_name;
      if (clip >= 0 && !wire::station_action_detail::clip_name_valid(action.bellows_clip) &&
          action.bellows_clip != refused_name) {
        refused_name = action.bellows_clip;
        log_line("SMITHING own bellows clip %d named \"%s\" cannot travel", clip,
                 action.bellows_clip.c_str());
      }
      if (!wire::station_action_detail::clip_name_valid(action.bellows_clip)) {
        action.bellows_clip.clear();
        action.bellows_flags = 0;
      }
      log_bellows(thing, action.bellows_clip);
      continue;
    }
    if (kParts[i].id == wire::StationPropId::Workpiece) {
      const std::string name = entity::name_of(thing, wire::kStationWorkpieceMax);
      if (wire::station_workpiece_model(name)) {
        action.workpiece = name;
      }
    }
    wire::StationProp prop;
    // The workpiece is measured against the tongs while they carry it (the item the tongs'
    // part moves).
    void* tongs = nullptr;
    if (kParts[i].id == wire::StationPropId::Workpiece) {
      for (std::size_t t = 0; t < kPartCount; ++t) {
        if (kParts[t].id != wire::StationPropId::Tongs) {
          continue;
        }
        const auto* tongs_attachment = read<const std::uint8_t*>(context, kParts[t].offset);
        void* tongs_slot =
            tongs_attachment
                ? entity::by_id(read<std::uint32_t>(tongs_attachment, kAttachmentEntity))
                : nullptr;
        // Only tongs in the smith's hand carry the piece.
        const bool in_hand = measured[t] && measured[t]->anchor != wire::StationAnchor::Station;
        tongs = tongs_slot && in_hand ? moved_by(t, tongs_slot) : nullptr;
      }
    }
    if (!measure(i, attachment, thing, body, station_position, station_rotation, tongs, now,
                 prop)) {
      continue;
    }
    // What moved less than a few millimetres stays as published, so the field does not change
    // with the measurement's noise.
    if (const wire::StationProp* before = prop_of(published, prop.id)) {
      if (same_place(*before, prop)) {
        prop = *before;
      }
    }
    action.props.push_back(prop);
  }
  read_effects(context, station_position, station_rotation, action);
  const std::string field = wire::station_action_field(action);
  if (field.empty()) {
    static double refused_at = -10.0;
    if (now - refused_at >= 5.0) {
      refused_at = now;
      log_line("SMITHING own action not writable: sparks %d %.2f,%.2f,%.2f x%.2f burning %zu",
               int(action.sparks_at), action.sparks[0], action.sparks[1], action.sparks[2],
               action.sparks[3], action.burning.size());
      for (const auto& prop : action.props) {
        log_line("SMITHING   prop %c%c %.3f,%.3f,%.3f", char(prop.id), char(prop.anchor),
                 prop.position[0], prop.position[1], prop.position[2]);
      }
    }
    return published_field;
  }
  if (field == published_field) {
    return published_field;
  }
  bool anchors_changed = published_field.empty() || action.workpiece != published.workpiece ||
                         action.props.size() != published.props.size();
  for (std::size_t i = 0; !anchors_changed && i < action.props.size(); ++i) {
    anchors_changed = action.props[i].id != published.props[i].id ||
                      action.props[i].anchor != published.props[i].anchor;
  }
  if (anchors_changed || now - published_at >= kPublishInterval) {
    if (anchors_changed) {
      log_line("SMITHING own action %s", field.c_str());
    }
    published = action;
    published_field = field;
    published_at = now;
  }
  return published_field;
}

void keep(std::uint32_t holder, void* body, const std::string& field, double now,
          const BodyMotion& motion) {
  frame_now = now;
  const std::string kind = std::string(wire::kStationKindSmith) + " ";
  if (!body || field.rfind(kind, 0) != 0) {
    forget(holder);
    return;
  }
  auto found = shown.find(holder);
  if (found != shown.end() && found->second.body != body) {
    release(found->second);
    shown.erase(found);
    found = shown.end();
  }
  if (found == shown.end()) {
    found = shown.emplace(holder, Shown{}).first;
    found->second.body = body;
    found->second.body_id = entity::id_of(body);
  }
  Shown& entry = found->second;
  if (field != entry.field) {
    wire::StationAction action;
    if (!wire::parse_station_action_field(field, action)) {
      log_line("SMITHING holder=%u unreadable action", holder);
      forget(holder);
      return;
    }
    if (entry.field.empty()) {
      log_line("SMITHING holder=%u smiths %s", holder, field.c_str());
    }
    if (distance(action.station.data(), entry.action.station.data()) > kStationMatch) {
      release(entry);
      entry.station = 0;
      entry.looked_for_tools = false;
    }
    entry.field = field;
    entry.action = std::move(action);
  }
  if (!find_station(holder, entry)) {
    return;
  }
  keep_tool(holder, entry, entry.hammer, entry.hammer_copy, kHammerClass, kHammerPrefix,
            wire::StationPropId::Hammer);
  keep_tool(holder, entry, entry.tongs, entry.tongs_copy, kTongsClass, kTongsPrefix,
            wire::StationPropId::Tongs);
  keep_bellows(holder, entry, motion);
  keep_effects(holder, entry);
  const char* model = wire::station_workpiece_model(entry.action.workpiece);
  const wire::StationProp* workpiece = prop_of(entry.action, wire::StationPropId::Workpiece);
  if (model && workpiece) {
    keep_copy(holder, entry, entry.workpiece_copy, model, std::string(), *workpiece);
    std::optional<Copy>& copy = entry.workpiece_copy;
    void* thing = copy && copy->placed ? entity::find(copy->name) : nullptr;
    // At most every kGlowEvery seconds (the game's own fade carries it between): applied as often
    // as the heat changed, eight times a second while it cooled, the sword blinked on other
    // screens. The first glow and a cold piece are applied at once.
    const bool cold = entry.action.glow.find_first_not_of('0') == std::string::npos;
    const bool due =
        copy && (copy->glow.empty() || cold || frame_now - copy->glow_at >= kGlowEvery);
    if (thing && copy->glow != entry.action.glow && due) {
      // Every zone again whenever one changes: a part's own material taken for one zone left the
      // other zones dark until their own next change (video: the horseshoe's glowing
      // parts went out and on many times a second on the anvil).
      for (std::size_t at = 0; at + 2 <= entry.action.glow.size(); at += 2) {
        glow_zone(thing, copy->glow_zones, at / 2,
                  wire::station_glow_parse(entry.action.glow.substr(at, 2)));
      }
      copy->glow = entry.action.glow;
      copy->glow_at = frame_now;
      log_line("SMITHING holder=%u glow %s", holder, copy->glow.c_str());
    }
    place_burning(entry, thing);
  } else {
    drop(entry.workpiece_copy);
  }
}

void forget(std::uint32_t holder) {
  auto found = shown.find(holder);
  if (found == shown.end()) {
    return;
  }
  log_line("SMITHING holder=%u stopped", holder);
  release(found->second);
  shown.erase(found);
}

void reset() {
  shown.clear();
  clear_owner();
  minigame = nullptr;
}

bool station_taken(const float position[3]) {
  for (const auto& [holder, entry] : shown) {
    if (distance(entry.action.station.data(), position) <= kStationMatch) {
      return true;
    }
  }
  return false;
}

std::string take_chosen_recipe(float station[3]) {
  std::memcpy(station, recipe_station.data(), sizeof(float) * 3);
  return std::exchange(recipe_to_report, {});
}

bool take_broken_workpiece(float station[3]) {
  std::memcpy(station, recipe_station.data(), sizeof(float) * 3);
  return std::exchange(broken_to_report, false);
}

bool own_running() {
  return smithing_now();
}

bool close_own() {
  if (!smithing_now()) {
    return false;
  }
  // At the recipe book no recipe is chosen yet, and the exit below reads the recipe's values
  // (-> ->) without a check. The book's own close action runs instead
  // (registered): it closes the screen and sets the state "no recipe", from
  // which the frame update ends the minigame. Its capture holds the minigame at + 8.
  if (recipe_id_of(minigame).empty()) {
    if (!close_recipe_book) {
      log_line("SMITHING the server closed the station, but the book's close is missing");
      return true;
    }
    struct BookCapture {
      void* unused = nullptr;
      void* minigame = nullptr;
    };
    BookCapture capture{nullptr, minigame};
    close_recipe_book(&capture);
    log_line("SMITHING the server closed the station at the recipe book");
    return true;
  }
  // What the minigame's exit action runs (its callback): the minigame's own
  // exit, vtable slot 46.
  using Exit = void(__fastcall*)(void* minigame);
  constexpr std::size_t kExitSlot = 46;
  auto table = *static_cast<void***>(minigame);
  reinterpret_cast<Exit>(table[kExitSlot])(minigame);
  log_line("SMITHING the server closed the station");
  return true;
}

}  // namespace kcds::runtime::crafting::smithing
