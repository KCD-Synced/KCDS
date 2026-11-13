#include "alchemy.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/game_layouts.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "MinHook.h"
#include "emitters.hpp"
#include "hand_effects.hpp"
#include "kcd2s/bridge/sigscan.hpp"
#include "sounds.hpp"
#include "station_clips.hpp"
#include "kcd2s/integration/station_action.hpp"

namespace kcds::runtime::crafting::alchemy {
namespace {

namespace wire = kcd2s::integration;
namespace entity = props::entity;

// C_Alchemy (see alchemy.hpp).
constexpr char kMinigameType[] = ".?AVC_Alchemy@playermodule@wh@@";
constexpr std::size_t kUpdateSlot = 19;
constexpr std::size_t kActor = 24;
constexpr std::size_t kStation = 40;
constexpr std::size_t kRunning = 44;
// The minigame's update is looked for this long before its player counts as away.
constexpr std::uint64_t kUpdateGraceMs = 1000;

// CEntity: its flags at + 8 (0x10 hidden, CEntity::Hide) and slot 108 LoadGeometry.
constexpr char kEntityType[] = ".?AVCEntity@@";
constexpr std::size_t kEntityFlags = 8;
constexpr std::uint32_t kEntityHidden = 0x10;
constexpr std::size_t kLoadGeometrySlot = 108;

// The table and its things, as every client's level has them; the recipe book on its stand is a
// RecipesBook (recipesbook_alchemy at Troskovice).
constexpr char kTableClass[] = "AlchemyTable";
constexpr const char* kThingClasses[] = {"AlchemyTable", "AlchemyItem", "ParticleEffect", "Light",
                                         "RecipesBook"};
// The things whose sounds are watched: the table (its fire and its boiling) and its items.
constexpr const char* kSoundClasses[] = {"AlchemyTable", "AlchemyItem"};
constexpr char kParticleClass[] = "ParticleEffect";
constexpr char kLightClass[] = "Light";
// The sounds the table plays on the brewer's game only, each while one of its particles burns
// (measured on the loopback, a watcher heard none of them before): the fire under the
// pot (its FireSound, the prefab's AlchemyTable property) and the boiling (the bubbles' sound).
// Another client plays them on the table, as the brewer's game does, while that particle burns.
struct ParticleSound {
  const char* key;
  const char* trigger;
};
constexpr ParticleSound kParticleSounds[] = {
    {"particle_fireplace_pot", "special_alchemy_fireplace"},
    {"particle_boiling_bubbles_pot", "special_alchemy_boiling_water"},
};
// The table's things stand within this of it (its farthest, the bellows, 2.5 m at Troskovice),
// and the reported table is found within kTableMatch of a client's own.
constexpr float kTableReach = 2.7f;
constexpr float kTableMatch = 1.5f;
// The things the minigame spawns (herbs) that one session reports at most.
constexpr std::size_t kSpawnsMax = 16;

// A particle component: its entity at + 40, whether it burns at + 108.
constexpr std::size_t kParticleBurns = 108;
// A light entity's light: its render nodes (slot 88 the count, slot 90 one by slot), the node's
// light (slot 75) and its colour at + 32.
constexpr std::size_t kSlotCount = 88;
constexpr std::size_t kSlotNode = 90;
constexpr std::size_t kNodeLight = 75;
constexpr std::size_t kLightColour = 32;
// Two light colours are alike while no channel is a quarter (or 0.05) off.
constexpr float kColourShare = 0.25f;
constexpr float kColourLeast = 0.05f;
// Spawn parameters travel in twentieths.
constexpr float kSpawnStep = 0.05f;
// The owner's client notes a table's things before its player uses it, once the player comes
// this near (the minigame lights the fire and lays things out as it starts).
constexpr float kApproach = 4.0f;
constexpr std::uint64_t kApproachLookMs = 1000;

// A thing moved less than this, or turned less than this (the quaternions' dot), is where it
// was; a published place is kept until the thing is farther from it than that.
constexpr float kMoved = 0.005f;
constexpr float kTurned = 0.9999f;
// The field changes at most this often while only places move (seconds).
constexpr double kPublishInterval = 0.1;
// A thing moved at the replica's table eases to its new place at this share per second (about
// 50 ms behind; a held thing does not ease, it hangs from the replica's hand).
constexpr double kEaseRate = 20.0;

using Update = void(__fastcall*)(void* minigame);
using LoadGeometry = int(__fastcall*)(void* entity, int slot, const char* file,
                                      const char* geometry, int flags);
using ParticleEnable = void*(__fastcall*)(void* component, bool on);
using ParticleComponent = void*(__fastcall*)(void* unused, void* entity);
using SlotCount = int(__fastcall*)(void* entity);
using SlotNode = void*(__fastcall*)(void* entity, int slot);
using Get = void*(__fastcall*)(void* object);

Natives natives;
Update original_update = nullptr;
LoadGeometry original_load_geometry = nullptr;

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

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

float distance(const float a[3], const float b[3]) {
  const float x = a[0] - b[0];
  const float y = a[1] - b[1];
  const float z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

float turn_between(const float a[4], const float b[4]) {
  return std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
}

bool hidden(void* thing) {
  return (read<std::uint32_t>(thing, kEntityFlags) & kEntityHidden) != 0;
}

void* particle_of(void* thing) {
  if (!thing || !natives.particle_component) {
    return nullptr;
  }
  return reinterpret_cast<ParticleComponent>(natives.particle_component)(nullptr, thing);
}

// Whether a thing's particles burn; -1 for a thing without particles.
int burns(void* thing) {
  void* component = particle_of(thing);
  if (!component) {
    return -1;
  }
  return read<bool>(component, kParticleBurns) ? 1 : 0;
}

void switch_particles(void* thing, bool on) {
  void* component = particle_of(thing);
  if (!component) {
    return;
  }
  if (on && natives.particle_activate) {
    reinterpret_cast<Get>(natives.particle_activate)(component);
  } else if (natives.particle_enable) {
    reinterpret_cast<ParticleEnable>(natives.particle_enable)(component, on);
  }
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

bool near_colour(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  for (int channel = 0; channel < 3; ++channel) {
    const float share = std::fabs(b[channel]) * kColourShare;
    const float allowed = share > kColourLeast ? share : kColourLeast;
    if (std::fabs(a[channel] - b[channel]) > allowed) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// The table's things, found the same way on every client.

struct Thing {
  std::uint32_t id = 0;
  std::string key;
  bool particles = false;
  bool light = false;
  bool sounds = false;
};

// A level entity's name before its "[" (the prefab's path), as a key.
std::string base_name(void* thing) {
  std::string name = entity::name_of(thing, 160);
  const std::size_t bracket = name.find('[');
  if (bracket != std::string::npos) {
    name.resize(bracket);
  }
  return name;
}

bool key_fits(const std::string& key) {
  if (key.empty() || key.size() > wire::kStationKeyMax || key[0] == '@' || key[0] == '#') {
    return false;
  }
  for (const char c : key) {
    if (c <= ' ' || c > '~') {
      return false;
    }
  }
  return true;
}

// The things standing at a table, keyed by their names; things of the same name get "~1",
// "~2", ... in the order of their places, which is the same on every client.
std::vector<Thing> things_at(const float table[3]) {
  struct Found {
    Thing thing;
    std::array<float, 3> place{};
  };
  std::vector<Found> found;
  for (const char* entity_class : kThingClasses) {
    entity::each_of_class(entity_class, [&](void* candidate) {
      Found one;
      entity::world_position(candidate, one.place.data());
      if (distance(one.place.data(), table) > kTableReach) {
        return;
      }
      one.thing.id = entity::id_of(candidate);
      one.thing.key = base_name(candidate);
      one.thing.particles = std::strcmp(entity_class, kParticleClass) == 0;
      one.thing.light = std::strcmp(entity_class, kLightClass) == 0;
      for (const char* sound_class : kSoundClasses) {
        one.thing.sounds = one.thing.sounds || std::strcmp(entity_class, sound_class) == 0;
      }
      found.push_back(std::move(one));
    });
  }
  std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
    if (a.thing.key != b.thing.key) {
      return a.thing.key < b.thing.key;
    }
    return a.place < b.place;
  });
  std::vector<Thing> things;
  std::unordered_map<std::string, int> seen;
  for (Found& one : found) {
    const int earlier = seen[one.thing.key]++;
    if (earlier > 0) {
      one.thing.key += "~" + std::to_string(earlier);
    }
    if (key_fits(one.thing.key)) {
      things.push_back(std::move(one.thing));
    }
  }
  return things;
}

// The nearest alchemy table within `reach` of a place, or null.
void* nearest_table(const float place[3], float reach) {
  void* best = nullptr;
  float best_distance = reach;
  entity::each_of_class(kTableClass, [&](void* candidate) {
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

// ---------------------------------------------------------------------------------------------
// The owner's side.

void* local_actor = nullptr;
// The local player's body (own_field), whose distance decides what of the tables is heard.
void* local_body = nullptr;

// How far from a table another player hears the minigame. The game made its sounds for the player
// at the table and does not fade them with distance (heard across the map);
// a little farther still before they stop, so the edge does not switch them on and off.
constexpr float kHearingReach = 30.0f;
constexpr float kHearingSlack = 5.0f;
void* minigame = nullptr;
std::uint64_t minigame_seen = 0;

void __fastcall hooked_update(void* object) {
  original_update(object);
  if (object && local_actor && read<void*>(object, kActor) == local_actor) {
    minigame = object;
    minigame_seen = GetTickCount64();
  }
}

bool at_table_now() {
  return minigame && GetTickCount64() - minigame_seen <= kUpdateGraceMs;
}

// A thing of the table as the session began, and the mesh the minigame last gave it.
struct Watched {
  Thing thing;
  bool hidden = false;
  float position[3] = {};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  int burns = -1;
  bool lit = false;  // a light, its colour noted
  std::array<float, 3> colour{};
  std::string model;
  void* audio = nullptr;                   // its audio component, watched
  std::vector<wire::StationSound> sounds;  // what the minigame played on it this session
};

// A thing the minigame spawned at the table, in the order it came.
struct Spawned {
  std::uint32_t id = 0;
  std::string model;
};

std::uint32_t own_table = 0;
float own_table_position[3] = {};
float own_table_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
std::vector<Watched> watched;
std::vector<Spawned> spawned;
wire::StationAction published;
std::string published_field;
double published_at = -1.0;

Watched* watched_by(std::uint32_t id) {
  for (Watched& one : watched) {
    if (one.thing.id == id) {
      return &one;
    }
  }
  return nullptr;
}

// The name the minigame spawns its herbs under.
constexpr char kResourceName[] = "AlchemyResource";

int __fastcall hooked_load_geometry(void* thing, int slot, const char* file, const char* geometry,
                                    int flags) {
  const int result = original_load_geometry(thing, slot, file, geometry, flags);
  if (!thing || !file || own_table == 0 || !at_table_now()) {
    return result;
  }
  const std::string model = file;
  if (model.empty() || model.size() > wire::kStationModelMax ||
      model.find(' ') != std::string::npos) {
    return result;
  }
  const std::uint32_t id = entity::id_of(thing);
  if (Watched* one = watched_by(id)) {
    one->model = model;
    log_line("ALCHEMY own %s shows %s (slot %d)", one->thing.key.c_str(), file, slot);
    return result;
  }
  // The minigame's own resources (the herbs in the mortar and the bowl: an entity it spawns as
  // "AlchemyResource" and gives the table's HerbsMortar, HerbsMortarMilled, HerbsBow, ... model)
  // get their model before they are placed, while they still stand at the world's
  // origin; anything else counts only at the table.
  const bool resource = entity::name_of(thing, 32).rfind(kResourceName, 0) == 0;
  float place[3];
  entity::world_position(thing, place);
  if (!resource && distance(place, own_table_position) > kTableReach) {
    return result;
  }
  for (Spawned& one : spawned) {
    if (one.id == id) {
      one.model = model;
      return result;
    }
  }
  if (spawned.size() < kSpawnsMax) {
    spawned.push_back(Spawned{id, model});
    log_line("ALCHEMY own spawned #%zu %s (slot %d)", spawned.size() - 1, file, slot);
  }
  return result;
}

// A session ended: the table stays noted while the player stays near it.
void end_session() {
  spawned.clear();
  std::vector<sounds::Heard> unheard;
  for (Watched& one : watched) {
    one.sounds.clear();
    sounds::take(one.audio, unheard);
  }
  published = {};
  published_field.clear();
  published_at = -1.0;
}

void clear_owner() {
  end_session();
  own_table = 0;
  watched.clear();
}

// Notes a table's things as they are before the player uses it.
void watch(void* table) {
  clear_owner();
  own_table = entity::id_of(table);
  entity::world_pose(table, own_table_position, own_table_rotation);
  for (const Thing& thing : things_at(own_table_position)) {
    void* found = entity::by_id(thing.id);
    if (!found) {
      continue;
    }
    Watched one;
    one.thing = thing;
    one.hidden = hidden(found);
    float position[3];
    float rotation[4];
    entity::world_pose(found, position, rotation);
    pose_math::relative(own_table_position, own_table_rotation, position, rotation, one.position,
                        one.rotation);
    one.burns = thing.particles ? burns(found) : -1;
    const float* colour = thing.light ? light_colour(found) : nullptr;
    if (colour) {
      one.lit = true;
      one.colour = {colour[0], colour[1], colour[2]};
    }
    if (thing.sounds) {
      one.audio = sounds::audio_of(found);
      sounds::watch(one.audio);
    }
    watched.push_back(std::move(one));
  }
  log_line("ALCHEMY own table %u: %zu things", own_table, watched.size());
}

// Notes the table the player comes near (looked for once a second); forgets it once the player
// is away from every table.
// The level's tables are found once (walking every entity is too dear for once a second) and
// again, at most once a minute, while one of them is gone or none was found.
constexpr std::uint64_t kTablesLookAgainMs = 60000;
std::uint64_t approach_looked_at = 0;
std::uint64_t tables_looked_at = 0;
std::vector<std::uint32_t> known_tables;

void* table_near(const float place[3]) {
  bool all_there = !known_tables.empty();
  for (const std::uint32_t id : known_tables) {
    all_there = all_there && entity::by_id(id) != nullptr;
  }
  const std::uint64_t tick = GetTickCount64();
  if (!all_there && (tables_looked_at == 0 || tick - tables_looked_at >= kTablesLookAgainMs)) {
    tables_looked_at = tick;
    known_tables.clear();
    entity::each_of_class(
        kTableClass, [&](void* candidate) { known_tables.push_back(entity::id_of(candidate)); });
  }
  for (const std::uint32_t id : known_tables) {
    void* table = entity::by_id(id);
    if (!table) {
      continue;
    }
    float there[3];
    entity::world_position(table, there);
    if (distance(there, place) <= kApproach) {
      return table;
    }
  }
  return nullptr;
}

void note_nearby(void* body) {
  const std::uint64_t tick = GetTickCount64();
  if (!body || tick - approach_looked_at < kApproachLookMs) {
    return;
  }
  approach_looked_at = tick;
  float place[3];
  entity::world_position(body, place);
  void* table = table_near(place);
  if (!table) {
    if (own_table != 0) {
      clear_owner();
    }
    return;
  }
  if (entity::id_of(table) != own_table) {
    watch(table);
  }
}

const wire::StationEntry* entry_of(const wire::StationAction& action, const std::string& key) {
  for (const auto& entry : action.entries) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

// A thing's place relative to the table, kept as published while it moved less than a few
// millimetres.
// The brewer's hands, and how close to one a thing is held (its origin within this of the joint).
constexpr const char* kHandJoints[] = {"RightHand", "LeftHand"};
constexpr char kHandAnchors[] = {'R', 'L'};
constexpr float kHeldReach = 0.15f;

// A thing in one of the brewer's hands goes relative to that hand's joint: placed
// relative to the table it floated beside the replica's hand, which is never quite where the
// owner's is. A thing the game holds in a hand slot is in that hand however far its origin lies
// from the joint (the recipe book and the bottles: their origins are farther than kHeldReach, and
// they floated by the replica's hand); near_counts: whether a thing only near a hand is held too.
// False when no hand holds it.
bool place_in_hand(void* thing, const float position[3], const float rotation[4], bool near_counts,
                   wire::StationEntry& entry) {
  if (!local_body) {
    return false;
  }
  const std::uint32_t id = entity::id_of(thing);
  for (std::size_t i = 0; i < 2; ++i) {
    const bool in_slot = id != 0 && natives.hand_entity && natives.hand_entity(i) == id;
    float hand_position[3];
    float hand_rotation[4];
    if (!entity::joint_pose(local_body, kHandJoints[i], hand_position, hand_rotation)) {
      continue;
    }
    if (!in_slot && (!near_counts || distance(hand_position, position) > kHeldReach)) {
      continue;
    }
    pose_math::relative(hand_position, hand_rotation, position, rotation, entry.position.data(),
                        entry.rotation.data());
    pose_math::normalize(entry.rotation.data());
    entry.hand = kHandAnchors[i];
    return true;
  }
  return false;
}

// in_hand: whether a thing near a hand counts as held. The minigame's herbs lie in the mortar or
// the bowl, which the grinding hand comes close to: they count as held only while their vessel is
// (carried to the cauldron and poured).
void place_of(void* thing, const std::string& key, wire::StationEntry& entry, bool in_hand) {
  float position[3];
  float rotation[4];
  entity::world_pose(thing, position, rotation);
  entry.placed = true;
  // The recipe book is never held (in_hand false): its origin lies 0.6 m from the hand that
  // carries it, and placed from the replica's hand it jumped about; it stands where the owner's
  // stands.
  if (in_hand && place_in_hand(thing, position, rotation, true, entry)) {
    return;
  }
  pose_math::relative(own_table_position, own_table_rotation, position, rotation,
                      entry.position.data(), entry.rotation.data());
  pose_math::normalize(entry.rotation.data());
  const wire::StationEntry* before = entry_of(published, key);
  if (before && before->placed &&
      distance(before->position.data(), entry.position.data()) <= kMoved &&
      turn_between(before->rotation.data(), entry.rotation.data()) >= kTurned) {
    entry.position = before->position;
    entry.rotation = before->rotation;
  }
}

float in_steps(float value) {
  const float stepped = std::round(value / kSpawnStep) * kSpawnStep;
  return stepped < 0.0f
             ? 0.0f
             : (stepped > wire::kStationStrengthMax ? wire::kStationStrengthMax : stepped);
}

// The spawn parameters the minigame gave a thing's particles (a fire burning up or down), kept
// as published until one of them is a step off.
void read_spawn(void* thing, wire::StationEntry& entry) {
  void* emitter = emitters::of(thing);
  emitters::watch(emitter);
  emitters::Given given;
  if (!emitters::given(emitter, given)) {
    return;
  }
  const wire::StationEntry* before = entry_of(published, entry.key);
  if (before && before->spawning && std::fabs(given.count - before->spawn[0]) < kSpawnStep &&
      std::fabs(given.strength - before->spawn[1]) < kSpawnStep) {
    entry.spawning = true;
    entry.spawn = before->spawn;
    return;
  }
  const std::array<float, 2> now{in_steps(given.count), in_steps(given.strength)};
  if (now[0] != 1.0f || now[1] != 1.0f) {
    entry.spawning = true;
    entry.spawn = now;
  }
}

// Counts the starts and stops the game made on a thing's audio since the last frame.
void count_sounds(Watched& one) {
  static std::vector<sounds::Heard> heard;
  sounds::take(one.audio, heard);
  for (const sounds::Heard& sound : heard) {
    wire::StationSound* counted = nullptr;
    for (wire::StationSound& known : one.sounds) {
      if (known.trigger == sound.trigger) {
        counted = &known;
      }
    }
    if (!counted && sound.trigger != 0 && one.sounds.size() < wire::kStationEntrySoundsMax) {
      one.sounds.push_back(wire::StationSound{sound.trigger, 0, 0});
      counted = &one.sounds.back();
    }
    if (!counted) {
      continue;
    }
    if (sound.start) {
      ++counted->starts;
    } else {
      ++counted->stops;
    }
  }
}

// How each thing differs from the session's start now.
// The herb in the brewer's hand, a handful from one of the table's sacks (found live): the
// minigame says whether the hand holds something (+ 872) and which of the table's places it came
// from (+ 672; 21 none, the three sacks 7, 8 and 9). The game shows the handful on the brewer's
// own character, which other clients do not see: it travels as a thing of the table ("#hand"),
// the game's bunch of alchemy herb leaves between the brewer's right thumb and forefinger, which
// pinch it (a herb item's own model is the sack it lies in on the ground).
constexpr std::size_t kHolding = 872;
constexpr std::size_t kHeldPlace = 672;
constexpr std::uint32_t kFirstSack = 7;
constexpr std::uint32_t kSacks = 3;
constexpr char kHandKey[] = "#hand";
constexpr char kHandful[] = "Objects/manmade/task_specific_props/alchemy/herbs/herb_leaves.cgf";
constexpr char kHerbHand[] = "RightHand";
constexpr char kHerbThumb[] = "RightHandThumb3";
constexpr char kHerbFinger[] = "RightHandIndex3";

// Where a body's right hand pinches the handful: between its thumb and its forefinger, turned as
// the hand. False without those joints.
bool pinch_pose(void* body, float position[3], float rotation[4]) {
  float thumb[3];
  float finger[3];
  float unused[4];
  if (!entity::joint_pose(body, kHerbHand, position, rotation) ||
      !entity::joint_pose(body, kHerbThumb, thumb, unused) ||
      !entity::joint_pose(body, kHerbFinger, finger, unused)) {
    return false;
  }
  for (int axis = 0; axis < 3; ++axis) {
    position[axis] = (thumb[axis] + finger[axis]) * 0.5f;
  }
  return true;
}

void add_held_herb(wire::StationAction& action, void* body) {
  if (!body || !read<bool>(minigame, kHolding) ||
      action.entries.size() >= wire::kStationEntriesMax) {
    return;
  }
  const auto held = read<std::uint32_t>(minigame, kHeldPlace);
  if (held < kFirstSack || held >= kFirstSack + kSacks) {
    return;
  }
  float position[3];
  float rotation[4];
  if (!pinch_pose(body, position, rotation)) {
    return;
  }
  wire::StationEntry entry;
  entry.key = kHandKey;
  entry.hidden = 0;
  entry.model = kHandful;
  entry.placed = true;
  pose_math::relative(own_table_position, own_table_rotation, position, rotation,
                      entry.position.data(), entry.rotation.data());
  action.entries.push_back(std::move(entry));
}

// The clip the table's bellows play (their handle pumped with Q), in the field's
// bellows clip (P): every other client starts the same clip on its own bellows.
void read_bellows(wire::StationAction& action) {
  for (const Watched& one : watched) {
    if (one.thing.key.find("bellows") == std::string::npos) {
      continue;
    }
    void* bellows = entity::by_id(one.thing.id);
    std::uint32_t flags = 0;
    const std::int32_t clip = clips::top_clip(clips::skeleton_of(bellows), flags);
    std::string name = clips::clip_name(clips::character_of(bellows), clip);
    if (!wire::station_action_detail::clip_name_valid(name)) {
      name.clear();
      flags = 0;
    }
    static std::string logged = "-";
    if (name != logged) {
      logged = name;
      log_line("ALCHEMY own bellows %s clip %s flags %u", one.thing.key.c_str(),
               name.empty() ? "(none)" : name.c_str(), flags);
    }
    action.bellows_clip = name;
    action.bellows_flags = flags;
    return;
  }
}

// The clip a thing's own character plays now (the recipe book opening), as the field
// names clips; the bellows travel by the field's bellows clip instead (read_bellows).
void read_clip(void* thing, wire::StationEntry& entry) {
  if (entry.key.find("bellows") != std::string::npos) {
    return;
  }
  void* skeleton = clips::skeleton_of(thing);
  if (!skeleton) {
    return;
  }
  std::uint32_t flags = 0;
  const std::int32_t clip = clips::top_clip(skeleton, flags);
  const std::string name = clips::clip_name(clips::character_of(thing), clip);
  if (wire::station_action_detail::clip_name_valid(name)) {
    entry.clip = name;
    entry.clip_flags = flags;
  }
}

// Whether the brewer carries the mortar or the bowl now (its entry hangs from a hand).
bool vessel_in_hand(const wire::StationAction& action) {
  for (const wire::StationEntry& entry : action.entries) {
    const bool vessel = entry.key.find("mortar") != std::string::npos ||
                        entry.key.find("bowl") != std::string::npos;
    if (vessel && entry.hand != 0) {
      return true;
    }
  }
  return false;
}

void read_entries(wire::StationAction& action) {
  for (Watched& one : watched) {
    count_sounds(one);
    void* thing = entity::by_id(one.thing.id);
    if (!thing || action.entries.size() >= wire::kStationEntriesMax) {
      continue;
    }
    wire::StationEntry entry;
    entry.key = one.thing.key;
    const bool now_hidden = hidden(thing);
    if (now_hidden != one.hidden) {
      entry.hidden = now_hidden ? 1 : 0;
    }
    float position[3];
    float rotation[4];
    float relative_position[3];
    float relative_rotation[4];
    entity::world_pose(thing, position, rotation);
    pose_math::relative(own_table_position, own_table_rotation, position, rotation,
                        relative_position, relative_rotation);
    // The shown recipe book's pose is told always, so every other client stands its book exactly
    // where the owner's stands.
    const bool shown_book = !now_hidden && entry.key.rfind("recipesbook", 0) == 0;
    if (shown_book || distance(relative_position, one.position) > kMoved ||
        turn_between(relative_rotation, one.rotation) < kTurned) {
      place_of(thing, entry.key, entry, !shown_book);
    }
    entry.model = one.model;
    // Particles that burn are told always (a fire the player lit before, which the other
    // clients' levels do not burn), and those put out since the table was noted.
    if (one.thing.particles) {
      const int now_burns = burns(thing);
      if (now_burns == 1 || (now_burns == 0 && one.burns == 1)) {
        entry.burns = static_cast<std::int8_t>(now_burns);
      }
      read_spawn(thing, entry);
    }
    const float* colour = one.lit ? light_colour(thing) : nullptr;
    if (colour) {
      // A light counts once it is a quarter off the colour it had (a candle's own flicker does
      // not, a fire lit or put out does), and keeps the colour told until it is that far from it.
      const std::array<float, 3> now{colour[0], colour[1], colour[2]};
      const wire::StationEntry* before = entry_of(published, entry.key);
      if (before && before->lit && near_colour(now, before->colour)) {
        entry.lit = true;
        entry.colour = before->colour;
      } else if (!near_colour(now, one.colour)) {
        entry.lit = true;
        entry.colour = now;
      }
    }
    entry.sounds = one.sounds;
    read_clip(thing, entry);
    const bool differs = entry.hidden >= 0 || entry.placed || !entry.model.empty() ||
                         entry.burns >= 0 || entry.lit || entry.spawning || !entry.sounds.empty() ||
                         !entry.clip.empty();
    if (differs) {
      action.entries.push_back(std::move(entry));
    }
  }
  for (std::size_t i = 0; i < spawned.size(); ++i) {
    void* thing = entity::by_id(spawned[i].id);
    if (!thing || action.entries.size() >= wire::kStationEntriesMax) {
      continue;
    }
    wire::StationEntry entry;
    entry.key = "#" + std::to_string(i);
    entry.hidden = hidden(thing) ? 1 : 0;
    entry.model = spawned[i].model;
    place_of(thing, entry.key, entry, vessel_in_hand(action));
    action.entries.push_back(std::move(entry));
  }
}

// ---------------------------------------------------------------------------------------------
// Every other client's side.

// A thing of the replica's table as the level had it, and what the action changed of it.
struct Kept {
  Thing thing;
  bool hidden = false;
  float position[3] = {};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  int burns = -1;
  float colour[4] = {};
  bool lit = false;  // colour saved
  bool hid = false;  // hidden or shown by the action
  bool moved = false;
  bool switched = false;
  bool dimmed = false;
  bool spawning = false;  // its particles given the owner's spawn parameters
  std::array<float, 2> spawn{1.0f, 1.0f};
  // The sounds' counts already played, and the triggers started and not stopped since.
  std::vector<wire::StationSound> heard;
  std::vector<std::uint32_t> sounding;
  // The table's sounds this particle has started on the table (kParticleSounds).
  std::vector<std::uint32_t> table_sounding;
  // Whether the local player is within hearing of the thing, and of its table.
  bool in_hearing = false;
  bool table_in_hearing = false;
  std::string model;  // the mesh the action gave it
  float shown_position[3] = {};
  float shown_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  // The clip the action had its character play, and whether this client started it.
  std::string clip;
  bool clip_started = false;
};

// A copy of a thing the owner's minigame spawned.
struct Copy {
  std::string name;
  std::string model;
  float shown_position[3] = {};
  float shown_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool placed = false;
};

struct Shown {
  std::string field;
  wire::StationAction action;
  std::uint32_t table = 0;
  float table_position[3] = {};
  float table_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  std::vector<Kept> things;
  std::unordered_map<std::string, Copy> copies;
  double eased_at = 0.0;
  // The replica's body this frame (a held thing hangs from its hand).
  void* body = nullptr;
  // The bellows' clip as the field last gave it, its id here, and whether this client started one
  // on them.
  std::string bellows_clip;
  std::int32_t bellows_id = -1;
  bool bellows_started = false;
};

std::unordered_map<std::uint32_t, Shown> shown;
unsigned generation = 0;

// Finds the replica's table among this client's own and notes its things as the level has them.
bool find_table(std::uint32_t holder, Shown& entry) {
  if (entry.table != 0 && entity::by_id(entry.table)) {
    return true;
  }
  void* table = nearest_table(entry.action.station.data(), kTableMatch);
  if (!table) {
    return false;
  }
  entry.table = entity::id_of(table);
  entity::world_pose(table, entry.table_position, entry.table_rotation);
  entry.things.clear();
  for (const Thing& thing : things_at(entry.table_position)) {
    void* found = entity::by_id(thing.id);
    if (!found) {
      continue;
    }
    Kept kept;
    kept.thing = thing;
    kept.hidden = hidden(found);
    entity::world_pose(found, kept.position, kept.rotation);
    std::memcpy(kept.shown_position, kept.position, sizeof(kept.position));
    std::memcpy(kept.shown_rotation, kept.rotation, sizeof(kept.rotation));
    kept.burns = thing.particles ? burns(found) : -1;
    const float* colour = thing.light ? light_colour(found) : nullptr;
    if (colour) {
      std::memcpy(kept.colour, colour, sizeof(kept.colour));
      kept.lit = true;
    }
    entry.things.push_back(std::move(kept));
  }
  log_line("ALCHEMY holder=%u table %u: %zu things", holder, entry.table, entry.things.size());
  for (const Kept& kept : entry.things) {
    if (kept.burns >= 0 || kept.lit) {
      log_line("ALCHEMY   %s burns %d light %.3f hidden %d", kept.thing.key.c_str(), kept.burns,
               kept.lit ? kept.colour[0] : -1.0f, int(kept.hidden));
    }
  }
  return true;
}

// Eases a shown place towards a target, at kEaseRate per second.
void ease(float shown_position[3], float shown_rotation[4], const float position[3],
          const float rotation[4], double since) {
  float share = static_cast<float>(since * kEaseRate);
  share = share < 0.0f ? 0.0f : (share > 1.0f ? 1.0f : share);
  for (int axis = 0; axis < 3; ++axis) {
    shown_position[axis] += (position[axis] - shown_position[axis]) * share;
  }
  const float dot = shown_rotation[0] * rotation[0] + shown_rotation[1] * rotation[1] +
                    shown_rotation[2] * rotation[2] + shown_rotation[3] * rotation[3];
  const float sign = dot < 0.0f ? -1.0f : 1.0f;
  for (int axis = 0; axis < 4; ++axis) {
    shown_rotation[axis] += (sign * rotation[axis] - shown_rotation[axis]) * share;
  }
  pose_math::normalize(shown_rotation);
}

void sound_on(Kept& kept, void* thing, std::uint32_t trigger, bool start);

// Whether the local player is within hearing of `thing`; `heard` is the answer of the frame before,
// for the margin.
bool within_hearing(void* thing, bool heard) {
  if (!thing || !local_body) {
    return false;
  }
  float here[3];
  float there[3];
  entity::world_position(local_body, here);
  entity::world_position(thing, there);
  return distance(here, there) <= (heard ? kHearingReach + kHearingSlack : kHearingReach);
}

// The table's sound that follows this particle (kParticleSounds), on the table while it burns.
void keep_particle_sound(Kept& kept, const Shown& table, bool burning) {
  const char* name = nullptr;
  for (const ParticleSound& sound : kParticleSounds) {
    if (kept.thing.key == sound.key) {
      name = sound.trigger;
    }
  }
  void* table_entity = name ? entity::by_id(table.table) : nullptr;
  const std::uint32_t trigger = table_entity ? sounds::trigger_id(name) : 0;
  if (trigger == 0) {
    return;
  }
  // Out of hearing the table is silent; coming back, a fire still burning is heard again.
  kept.table_in_hearing = within_hearing(table_entity, kept.table_in_hearing);
  burning = burning && kept.table_in_hearing;
  auto playing = std::find(kept.table_sounding.begin(), kept.table_sounding.end(), trigger);
  const bool sounding = playing != kept.table_sounding.end();
  if (burning == sounding) {
    return;
  }
  sounds::play(table_entity, trigger, burning);
  log_line("ALCHEMY %s table sound %s %s", kept.thing.key.c_str(), name,
           burning ? "started" : "stopped");
  if (burning) {
    kept.table_sounding.push_back(trigger);
  } else {
    kept.table_sounding.erase(playing);
  }
}

// One of the table's things as the entry says (or as the level had it without one).
// The table's recipe book (a RecipesBook entity, recipesbook_alchemy at Troskovice).
bool is_book(const Kept& kept) {
  return kept.thing.key.rfind("recipesbook", 0) == 0;
}

// The camera distance a table thing's character is run at on this client (clips::process): near,
// for its full detail.
constexpr float kProcessDistance = 5.0f;

// CryCharAnimationParams' CA_REPEAT_LAST_KEY: the clip holds its last frame once it ran (the
// recipe book stays open).
constexpr std::uint32_t kRepeatLastKey = 0x4;

// The clip the owner's thing plays, started on this client's thing as it changes.
// The game does not run a level entity's clip on another client (smithing.hpp), but a start puts
// the thing in the clip's pose at its key time: a clip that holds its last frame (the book
// opening) is started at its end, so the thing shows as the owner's does once the clip ran.
void keep_clip(Kept& kept, const wire::StationEntry* entry, void* thing) {
  const std::string clip = entry ? entry->clip : std::string();
  if (clip == kept.clip) {
    return;
  }
  void* skeleton = clips::skeleton_of(thing);
  kept.clip = clip;
  if (clip.empty()) {
    if (kept.clip_started) {
      clips::stop(skeleton);
      kept.clip_started = false;
    }
    return;
  }
  const std::int32_t id = clips::clip_id(clips::character_of(thing), clip);
  const bool holds = (entry->clip_flags & kRepeatLastKey) != 0;
  const std::uint32_t flags = entry->clip_flags | clips::kStartAtKeyTime;
  kept.clip_started = id >= 0 && clips::start(skeleton, id, flags, 0.0f, holds ? 1.0f : 0.0f);
  log_line("ALCHEMY thing %s plays %s%s: %s", kept.thing.key.c_str(), clip.c_str(),
           holds ? " (held at its end)" : "", kept.clip_started ? "started" : "refused");
}

// A table thing at a world pose. The recipe book goes through its world matrix: its own position
// is not the world's (set as one, the watcher's book stood off in the world).
void place_thing(const Kept& kept, void* thing, const float position[3], const float rotation[4]) {
  if (is_book(kept)) {
    entity::place_in_world(thing, position, rotation);
  } else {
    entity::place_rotated(thing, position, rotation);
  }
}

void keep_thing(Kept& kept, const wire::StationEntry* entry, const Shown& table, double since) {
  void* thing = entity::by_id(kept.thing.id);
  if (!thing) {
    return;
  }
  const bool want_hidden = entry && entry->hidden >= 0 ? entry->hidden == 1 : kept.hidden;
  if (hidden(thing) != want_hidden) {
    entity::hide(thing, want_hidden);
  }
  keep_clip(kept, entry, thing);
  // The owner's minigame opens the recipe book through the brewer's animation (the "alchemyBook"
  // scope context); nothing runs the book's character on this client, so it stood closed (or not
  // drawn at all) on its stand. One frame of it here each frame poses it as the owner's
  if (is_book(kept) && kept.clip_started && !want_hidden) {
    clips::process(thing, kProcessDistance);
  }
  kept.hid = entry && entry->hidden >= 0;
  if (entry && !entry->model.empty() && entry->model != kept.model) {
    slot_of<LoadGeometry>(thing, kLoadGeometrySlot)(thing, 0, entry->model.c_str(), nullptr, 0);
    kept.model = entry->model;
  }
  if (entry && entry->placed && entry->hand != 0) {
    // Held: in the replica's own hand, every frame, without easing.
    float hand_position[3];
    float hand_rotation[4];
    const char* joint = entry->hand == 'L' ? "LeftHand" : "RightHand";
    if (table.body && entity::joint_pose(table.body, joint, hand_position, hand_rotation)) {
      pose_math::compose(hand_position, hand_rotation, entry->position.data(),
                         entry->rotation.data(), kept.shown_position, kept.shown_rotation);
      entity::place_rotated(thing, kept.shown_position, kept.shown_rotation);
      kept.moved = true;
    }
  } else if (entry && entry->placed) {
    float position[3];
    float rotation[4];
    pose_math::compose(table.table_position, table.table_rotation, entry->position.data(),
                       entry->rotation.data(), position, rotation);
    if (!kept.moved) {
      std::memcpy(kept.shown_position, kept.position, sizeof(kept.position));
      std::memcpy(kept.shown_rotation, kept.rotation, sizeof(kept.rotation));
    }
    ease(kept.shown_position, kept.shown_rotation, position, rotation, since);
    place_thing(kept, thing, kept.shown_position, kept.shown_rotation);
    kept.moved = true;
  } else if (kept.moved) {
    place_thing(kept, thing, kept.position, kept.rotation);
    kept.moved = false;
  }
  if (kept.burns >= 0) {
    const int want = entry && entry->burns >= 0 ? entry->burns : kept.burns;
    bool restarted = false;
    if (burns(thing) != want) {
      switch_particles(thing, want == 1);
      restarted = true;
      log_line("ALCHEMY %s particles %s (emitter %p)", kept.thing.key.c_str(),
               want == 1 ? "on" : "off", emitters::of(thing));
    }
    kept.switched = entry && entry->burns >= 0;
    keep_particle_sound(kept, table, want == 1);
    // A restarted emitter takes its parameters again.
    if (entry && entry->spawning && (restarted || !kept.spawning || entry->spawn != kept.spawn)) {
      emitters::give(thing, emitters::Given{entry->spawn[0], entry->spawn[1]});
      kept.spawning = true;
      kept.spawn = entry->spawn;
    } else if ((!entry || !entry->spawning) && kept.spawning) {
      emitters::give(thing, emitters::Given{});
      kept.spawning = false;
      kept.spawn = {1.0f, 1.0f};
    }
  }
  if (kept.lit) {
    float* colour = light_colour(thing);
    if (colour && entry && entry->lit) {
      std::memcpy(colour, entry->colour.data(), 3 * sizeof(float));
      kept.dimmed = true;
    } else if (colour && kept.dimmed) {
      std::memcpy(colour, kept.colour, sizeof(kept.colour));
      kept.dimmed = false;
    }
  }
}

void sound_on(Kept& kept, void* thing, std::uint32_t trigger, bool start) {
  sounds::play(thing, trigger, start);
  log_line("ALCHEMY %s sound %08x %s", kept.thing.key.c_str(), static_cast<unsigned>(trigger),
           start ? "started" : "stopped");
  auto playing = std::find(kept.sounding.begin(), kept.sounding.end(), trigger);
  if (start && playing == kept.sounding.end()) {
    kept.sounding.push_back(trigger);
  } else if (!start && playing != kept.sounding.end()) {
    kept.sounding.erase(playing);
  }
}

// The sounds the owner's minigame played on a thing: every start and stop since the last field.
// A sound not heard before is started when it was started more often than stopped (a fire's loop
// a late joiner comes into, a sound new since the last field).
void keep_sounds(Kept& kept, const wire::StationEntry* entry) {
  void* thing = entity::by_id(kept.thing.id);
  if (!thing || !entry) {
    return;
  }
  const bool heard = within_hearing(thing, kept.in_hearing);
  if (heard != kept.in_hearing) {
    kept.in_hearing = heard;
    log_line("ALCHEMY thing=%u %s hearing", kept.thing.id, heard ? "in" : "out of");
    if (!heard) {
      const std::vector<std::uint32_t> playing = kept.sounding;
      for (const std::uint32_t trigger : playing) {
        sound_on(kept, thing, trigger, false);
      }
    } else {
      // Back in hearing: what the owner's game still plays (started more often than stopped).
      for (const wire::StationSound& sound : entry->sounds) {
        if (sound.starts != sound.stops) {
          sound_on(kept, thing, sound.trigger, true);
        }
      }
    }
  }
  for (const wire::StationSound& sound : entry->sounds) {
    const wire::StationSound* before = nullptr;
    for (const wire::StationSound& known : kept.heard) {
      if (known.trigger == sound.trigger) {
        before = &known;
      }
    }
    if (!before) {
      if (sound.starts != sound.stops && kept.in_hearing) {
        sound_on(kept, thing, sound.trigger, true);
      }
      continue;
    }
    const int stops = static_cast<std::uint8_t>(sound.stops - before->stops);
    const int starts = static_cast<std::uint8_t>(sound.starts - before->starts);
    const bool playing =
        std::find(kept.sounding.begin(), kept.sounding.end(), sound.trigger) != kept.sounding.end();
    if (stops > 0 && playing) {
      sound_on(kept, thing, sound.trigger, false);
    }
    if (starts > 0 && kept.in_hearing) {
      sound_on(kept, thing, sound.trigger, true);
    }
  }
  kept.heard = entry->sounds;
}

// A thing the owner's minigame spawned, shown at the replica's table.
void keep_copy(std::uint32_t holder, Shown& table, const wire::StationEntry& entry, double since) {
  Copy& copy = table.copies[entry.key];
  if (!copy.name.empty() && copy.model != entry.model) {
    entity::remove(copy.name);
    copy = Copy{};
  }
  float position[3];
  float rotation[4];
  float hand_position[3];
  float hand_rotation[4];
  const char* joint = entry.hand == 'L' ? "LeftHand" : "RightHand";
  const bool held = entry.hand != 0 && table.body &&
                    entity::joint_pose(table.body, joint, hand_position, hand_rotation);
  if (held) {
    pose_math::compose(hand_position, hand_rotation, entry.position.data(), entry.rotation.data(),
                       position, rotation);
  } else {
    pose_math::compose(table.table_position, table.table_rotation, entry.position.data(),
                       entry.rotation.data(), position, rotation);
  }
  if (copy.name.empty()) {
    if (entry.model.empty()) {
      return;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "kcds_alchemy_%u_%u", holder, ++generation);
    if (!entity::spawn_mesh(name, position, entry.model, std::string())) {
      log_line("ALCHEMY holder=%u could not show %s", holder, entry.model.c_str());
      return;
    }
    copy.name = name;
    copy.model = entry.model;
  }
  void* thing = entity::find(copy.name);
  if (!thing) {
    return;
  }
  if (!copy.placed || held) {
    // A held thing follows the replica's hand without easing.
    std::memcpy(copy.shown_position, position, sizeof(position));
    std::memcpy(copy.shown_rotation, rotation, sizeof(rotation));
    copy.placed = true;
  } else {
    ease(copy.shown_position, copy.shown_rotation, position, rotation, since);
  }
  entity::place_rotated(thing, copy.shown_position, copy.shown_rotation);
  const bool want_hidden = entry.hidden == 1;
  if (hidden(thing) != want_hidden) {
    entity::hide(thing, want_hidden);
  }
}

// The handful in the brewer's hand stands where the replica's own fingers pinch, every frame: put
// where the owner's hand was relative to the table it floated beside the replica's hand, which is
// never quite where the owner's is.
void pin_to_hand(Shown& table, void* body) {
  auto found = table.copies.find(kHandKey);
  if (found == table.copies.end() || found->second.name.empty()) {
    return;
  }
  void* copy = entity::find(found->second.name);
  float position[3];
  float rotation[4];
  if (copy && pinch_pose(body, position, rotation)) {
    entity::place_rotated(copy, position, rotation);
  }
}

// The table's bellows play the clip the owner's bellows play (a pump of the handle).
// The game does not run the bellows' clip on another client (its time stands still, as the forge
// bag's in smithing.hpp; live: a started pump showed its first pose only), so while it plays it is
// started again every frame at the body's own clip time: the bellows follow the replica's pull.
void keep_bellows(std::uint32_t holder, Shown& table, void* body) {
  void* bellows = nullptr;
  for (const Kept& kept : table.things) {
    if (kept.thing.key.find("bellows") != std::string::npos) {
      bellows = entity::by_id(kept.thing.id);
      break;
    }
  }
  void* skeleton = clips::skeleton_of(bellows);
  const std::string& clip = table.action.bellows_clip;
  if (clip.empty()) {
    if (table.bellows_started) {
      clips::stop(skeleton);
      table.bellows_started = false;
    }
    table.bellows_clip.clear();
    return;
  }
  if (clip != table.bellows_clip) {
    table.bellows_clip = clip;
    table.bellows_id = clips::clip_id(clips::character_of(bellows), clip);
    log_line("ALCHEMY holder=%u bellows play %s (%d)", holder, clip.c_str(), table.bellows_id);
  }
  if (table.bellows_id < 0 || !skeleton) {
    return;
  }
  clips::clear(skeleton);
  const std::uint32_t flags = table.action.bellows_flags | clips::kStartAtKeyTime;
  const float time = clips::top_clip_time(body);
  const bool started = clips::start(skeleton, table.bellows_id, flags, 0.0f, time);
  // Nothing runs the bellows' character on this client: one frame of it here, so the clip just
  // started poses the bellows.
  if (started) {
    clips::process(bellows, kProcessDistance);
  }
  if (started != table.bellows_started) {
    std::uint32_t now_flags = 0;
    const std::int32_t now_clip = clips::top_clip(skeleton, now_flags);
    log_line("ALCHEMY holder=%u bellows %s at %.2f: top clip %d flags %u", holder,
             started ? "driven" : "refused", time, now_clip, now_flags);
  }
  table.bellows_started = started;
}

// Everything the action changed back as the level had it, and the copies gone.
void release(Shown& table) {
  void* table_entity = entity::by_id(table.table);
  for (Kept& kept : table.things) {
    if (table_entity) {
      for (const std::uint32_t trigger : kept.table_sounding) {
        sounds::play(table_entity, trigger, false);
      }
    }
    void* thing = entity::by_id(kept.thing.id);
    if (!thing) {
      continue;
    }
    if (kept.hid && hidden(thing) != kept.hidden) {
      entity::hide(thing, kept.hidden);
    }
    if (kept.moved) {
      place_thing(kept, thing, kept.position, kept.rotation);
    }
    if (kept.spawning) {
      emitters::give(thing, emitters::Given{});
    }
    for (const std::uint32_t trigger : kept.sounding) {
      sounds::play(thing, trigger, false);
    }
    if (kept.switched && burns(thing) != kept.burns) {
      switch_particles(thing, kept.burns == 1);
    }
    float* colour = kept.dimmed ? light_colour(thing) : nullptr;
    if (colour) {
      std::memcpy(colour, kept.colour, sizeof(kept.colour));
    }
  }
  for (auto& [key, copy] : table.copies) {
    if (!copy.name.empty()) {
      entity::remove(copy.name);
    }
  }
  // The bellows and the things whose clip this client started back at rest as the level has them.
  for (const Kept& kept : table.things) {
    const bool bellows = kept.thing.key.find("bellows") != std::string::npos;
    if ((table.bellows_started && bellows) || kept.clip_started) {
      clips::stop(clips::skeleton_of(entity::by_id(kept.thing.id)));
    }
  }
  table.bellows_clip.clear();
  table.bellows_started = false;
  table.things.clear();
  table.copies.clear();
  table.table = 0;
}

bool is_alchemy(const std::string& field) {
  const std::string kind = std::string(wire::kStationKindAlchemy) + " ";
  return field.rfind(kind, 0) == 0;
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  const auto base = reinterpret_cast<std::uintptr_t>(natives.image);
  const std::size_t at =
      kcd2s::bridge::rtti_vtable(natives.image, natives.image_size, base, kMinigameType);
  if (at == kcd2s::bridge::knpos) {
    log_line("ALCHEMY the minigame's vtable is missing");
    return false;
  }
  void* target = read<void*>(natives.image, at + kUpdateSlot * sizeof(void*));
  if (!target ||
      MH_CreateHook(target, reinterpret_cast<void*>(&hooked_update),
                    reinterpret_cast<void**>(&original_update)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    log_line("ALCHEMY could not hook the minigame's update %p", target);
    return false;
  }
  // The meshes the minigame gives the table's things and the herbs it spawns; without it those
  // keep the level's meshes on every other client.
  const std::size_t entity_at =
      kcd2s::bridge::rtti_vtable(natives.image, natives.image_size, base, kEntityType);
  void* load = entity_at == kcd2s::bridge::knpos
                   ? nullptr
                   : read<void*>(natives.image, entity_at + kLoadGeometrySlot * sizeof(void*));
  if (!load ||
      MH_CreateHook(load, reinterpret_cast<void*>(&hooked_load_geometry),
                    reinterpret_cast<void**>(&original_load_geometry)) != MH_OK ||
      MH_EnableHook(load) != MH_OK) {
    log_line("ALCHEMY could not hook LoadGeometry %p", load);
  }
  log_line("ALCHEMY minigame update %p hooked", target);
  return true;
}

std::string own_field(void* actor, void* body, double now) {
  local_actor = actor;
  local_body = body;
  if (!actor || !body || !at_table_now() || read<void*>(minigame, kActor) != actor ||
      !read<bool>(minigame, kRunning)) {
    if (!published_field.empty()) {
      log_line("ALCHEMY own action ended");
    }
    minigame = nullptr;
    end_session();
    note_nearby(body);
    return {};
  }
  const std::uint32_t table_id = read<std::uint32_t>(minigame, kStation);
  void* table = entity::by_id(table_id);
  if (!table) {
    return published_field;
  }
  if (own_table != table_id) {
    watch(table);
  }
  wire::StationAction action;
  action.kind = wire::kStationKindAlchemy;
  std::memcpy(action.station.data(), own_table_position, sizeof(own_table_position));
  read_entries(action);
  add_held_herb(action, body);
  hand_effects::add_own(action, entity::id_of(body));
  read_bellows(action);
  const std::string field = wire::station_action_field(action);
  if (field.empty() || field == published_field) {
    return published_field;
  }
  bool keys_changed = published_field.empty() || action.entries.size() != published.entries.size();
  for (std::size_t i = 0; !keys_changed && i < action.entries.size(); ++i) {
    keys_changed = action.entries[i].key != published.entries[i].key;
  }
  if (keys_changed || now - published_at >= kPublishInterval) {
    if (keys_changed) {
      log_line("ALCHEMY own action %s", field.c_str());
    }
    published = std::move(action);
    published_field = field;
    published_at = now;
  }
  return published_field;
}

void keep(std::uint32_t holder, void* body, const std::string& field, double now) {
  if (!body || !is_alchemy(field)) {
    forget(holder);
    return;
  }
  Shown& entry = shown[holder];
  entry.body = body;
  if (field != entry.field) {
    wire::StationAction action;
    if (!wire::parse_station_action_field(field, action)) {
      log_line("ALCHEMY holder=%u unreadable action", holder);
      forget(holder);
      return;
    }
    if (entry.field.empty()) {
      log_line("ALCHEMY holder=%u at a table: %s", holder, field.c_str());
    }
    if (distance(action.station.data(), entry.action.station.data()) > kTableMatch) {
      release(entry);
    }
    entry.field = field;
    entry.action = std::move(action);
  }
  hand_effects::keep(holder, body, entry.action);
  if (!find_table(holder, entry)) {
    return;
  }
  const double since = entry.eased_at > 0.0 ? now - entry.eased_at : 1.0;
  entry.eased_at = now;
  for (Kept& kept : entry.things) {
    const wire::StationEntry* thing_entry = entry_of(entry.action, kept.thing.key);
    keep_thing(kept, thing_entry, entry, since);
    keep_sounds(kept, thing_entry);
  }
  keep_bellows(holder, entry, body);
  for (const auto& thing_entry : entry.action.entries) {
    if (!thing_entry.key.empty() && thing_entry.key[0] == '#' && thing_entry.placed &&
        !hand_effects::is_effect_key(thing_entry.key)) {
      keep_copy(holder, entry, thing_entry, since);
      if (thing_entry.key == kHandKey) {
        pin_to_hand(entry, body);
      }
    }
  }
  for (auto it = entry.copies.begin(); it != entry.copies.end();) {
    if (!entry_of(entry.action, it->first)) {
      if (!it->second.name.empty()) {
        entity::remove(it->second.name);
      }
      it = entry.copies.erase(it);
    } else {
      ++it;
    }
  }
}

void forget(std::uint32_t holder) {
  auto found = shown.find(holder);
  if (found == shown.end()) {
    return;
  }
  log_line("ALCHEMY holder=%u left the table", holder);
  hand_effects::forget(holder);
  release(found->second);
  shown.erase(found);
}

void reset() {
  hand_effects::reset();
  shown.clear();
  clear_owner();
  minigame = nullptr;
  known_tables.clear();
  tables_looked_at = 0;
}

bool table_taken(const float position[3]) {
  for (const auto& [holder, entry] : shown) {
    if (distance(entry.action.station.data(), position) <= kTableMatch) {
      return true;
    }
  }
  return false;
}

bool own_running() {
  return at_table_now();
}

bool table_contents(wire::CraftTable& table) {
  constexpr std::size_t kResources = 1136;
  constexpr std::size_t kResourceList = 24;
  constexpr std::size_t kResourceContainer = 8;
  constexpr std::size_t kContainerFirst = 8;
  constexpr std::size_t kResourceFlags = 16;
  constexpr std::size_t kResourceBase = 32;
  constexpr std::size_t kItemClass = kcds::layouts::ItemInstance::kClass;
  constexpr std::size_t kClassGuid = 8;
  constexpr std::size_t kMostOnePlace = 32;
  if (!at_table_now()) {
    return false;
  }
  table.items.clear();
  const auto* game = static_cast<const std::uint8_t*>(minigame);
  for (std::uint8_t place = 0; place < wire::kCraftTablePositions; ++place) {
    const std::size_t list = kResources + kResourceList * place;
    const auto first = read<void**>(game, list);
    const auto last = read<void**>(game, list + 8);
    if (!first || last < first || std::size_t(last - first) > kMostOnePlace) {
      continue;
    }
    for (void** entry = first; entry != last; ++entry) {
      const auto* resource = static_cast<const std::uint8_t*>(*entry);
      if (!resource || table.items.size() >= wire::kMaxCraftTableItems) {
        continue;
      }
      void* container = read<void*>(resource, kResourceContainer);
      void* item = container ? read<void*>(container, kContainerFirst) : nullptr;
      void* item_class = item ? read<void*>(item, kItemClass) : nullptr;
      wire::CraftTableItem thing;
      thing.position = place;
      if (item_class) {
        std::memcpy(thing.item.data(), static_cast<const std::uint8_t*>(item_class) + kClassGuid,
                    thing.item.size());
      }
      const auto flags = read<std::uint32_t>(resource, kResourceFlags);
      thing.milled = (flags & 1u) != 0;
      thing.distilled = (flags & 2u) != 0;
      thing.base = read<std::uint8_t>(resource, kResourceBase) != 0;
      table.items.push_back(thing);
    }
  }
  return true;
}

bool close_own() {
  if (!at_table_now()) {
    return false;
  }
  // What the minigame's exit action runs (its callback): the minigame's own
  // exit, vtable slot 46.
  using Exit = void(__fastcall*)(void* minigame);
  constexpr std::size_t kExitSlot = 46;
  auto table = *static_cast<void***>(minigame);
  reinterpret_cast<Exit>(table[kExitSlot])(minigame);
  log_line("ALCHEMY the server closed the station");
  return true;
}

}  // namespace kcds::runtime::crafting::alchemy
