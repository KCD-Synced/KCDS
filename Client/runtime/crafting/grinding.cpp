#include "grinding.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "MinHook.h"
#include "emitters.hpp"
#include "sounds.hpp"
#include "station_clips.hpp"
#include "kcd2s/bridge/sigscan.hpp"
#include "kcd2s/integration/station_action.hpp"

namespace kcds::runtime::crafting::grinding {
namespace {

namespace wire = kcd2s::integration;
namespace entity = props::entity;

// C_Sharpening (see grinding.hpp).
constexpr char kMinigameType[] = ".?AVC_Sharpening@playermodule@wh@@";
constexpr std::size_t kUpdateSlot = 19;
constexpr std::size_t kActor = 24;
constexpr std::size_t kRunning = 44;
constexpr std::size_t kGrindstone = 224;
constexpr std::size_t kWheelSpeed = 172;
constexpr std::size_t kSparksEmitter = 272;
constexpr std::size_t kFailEmitter = 280;

// The grindstone as every client's level has it, and the minigame's sparks at its wheel.
constexpr char kGrindstoneClass[] = "Grindstone";
constexpr char kSparksEffect[] = "particles_smithery.particles_smithery.sharpening";
constexpr char kFailEffect[] = "particles_smithery.particles_smithery.sharpening_fail";
constexpr char kSparksJoint[] = "sword_pos";
// The sharpener's pedalling is the blend space whose time the game gives the wheel (the
// minigame looks it up as "2d_sharpening", which a replica's animation set did not find, live, and
// its id at + 404 was no clip either): the clip the sharpener's layer 0 plays while the wheel
// turns, which the replica plays by the same id. Where an emitter stands (IParticleEmitter: its
// rotation at + 112, its position at + 128).
constexpr std::size_t kEmitterRotation = 112;
constexpr std::size_t kEmitterPosition = 128;
constexpr float kPlaceSteps = 1000.0f;

// How far from the reported place this client's grindstone may stand.
constexpr float kGrindstoneMatch = 1.5f;
// Without an update for this long the minigame has ended.
constexpr std::uint64_t kUpdateGraceMs = 500;
// The field changes at most this often while only the sparks and the sound's parameters change.
constexpr double kPublishInterval = 0.1;
// The camera distance the grindstone's character is run at on this client (clips::process).
constexpr float kProcessDistance = 5.0f;
// The sparks' count scale travels in whole steps, their strength and the audio parameters in
// hundredths.
constexpr float kFineSteps = 100.0f;

// The particle manager's FindEffect and DeleteEmitter (as the minigame removes its sparks),
// and an effect's Spawn (as the effect attachment spawns its emitter).
constexpr std::size_t kFindEffect = 6;
constexpr std::size_t kDeleteEmitter = 14;
constexpr std::size_t kSpawnEmitter = 6;
// ISkeletonAnim's layers' playback scales (SetLayerPlaybackScale, slot 23: a layer's
// scale at + 80 + 64 * layer). The minigame's body action (C_HumanSharpeningAction, minigame
// + 520, the wheel's speed at + 92) runs the pedalling at the wheel's speed; the replica's clips
// arrive without it.
constexpr std::size_t kLayerPlaybackScale = 23;
constexpr std::size_t kLayerScales = 80;
constexpr std::size_t kLayerStride = 64;
constexpr std::size_t kLayers = 16;
constexpr float kScaleUnchanged = 0.005f;

using Update = void(__fastcall*)(void* minigame, float frame_time);
using DeleteEmitter = void(__fastcall*)(void* manager, void* emitter);
using Get = void*(__fastcall*)(void* object);
using SetLayerScale = void(__fastcall*)(void* skeleton, std::uint32_t layer, float scale);
// IParticleEmitter::SetLocation (slot 86), as the effect attachment places its emitter at the
// attachment every time the character is processed: a QuatTS.
struct Location {
  float rotation[4];
  float position[3];
  float scale;
};
using SetLocation = void(__fastcall*)(void* emitter, const Location* location);
using FindEffect = void*(__fastcall*)(void* manager, const char* name, const char* source,
                                      bool load);
using SpawnEmitter = void*(__fastcall*)(void* effect, const Location* location,
                                        std::uint32_t flags);
constexpr std::size_t kSetLocation = 86;

Natives natives;
Update original_update = nullptr;

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

float in_steps(float value, float steps) {
  return std::round(value * steps) / steps;
}

// ---------------------------------------------------------------------------------------------
// The owner's side.

// The minigame the local player's game runs, as its update last saw it.
void* local_actor = nullptr;
void* minigame = nullptr;
std::uint64_t minigame_seen = 0;

void __fastcall hooked_update(void* object, float frame_time) {
  original_update(object, frame_time);
  if (object && local_actor && read<void*>(object, kActor) == local_actor) {
    minigame = object;
    minigame_seen = GetTickCount64();
  }
}

bool grinding_now() {
  return minigame && GetTickCount64() - minigame_seen <= kUpdateGraceMs &&
         read<void*>(minigame, kActor) == local_actor && read<bool>(minigame, kRunning);
}

// What the owner's client published, and when; the sounds counted since the session began.
std::string published_field;
wire::StationAction published;
double published_at = -1.0;
std::uint32_t watched_grindstone = 0;
std::vector<wire::StationSound> wheel_sounds;
std::string last_clip;
float last_speed = -1.0f;
std::vector<wire::StationLayerScale> last_scales;
std::string pedal_clip;  // the sharpener's pedalling, once seen

// Whether two lists scale the same layers (whatever the scales).
bool same_layers(const std::vector<wire::StationLayerScale>& a,
                 const std::vector<wire::StationLayerScale>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].layer != b[i].layer) {
      return false;
    }
  }
  return true;
}

// The body's layers whose playback scale is not 1.
std::vector<wire::StationLayerScale> layer_scales_of(void* body) {
  std::vector<wire::StationLayerScale> scales;
  void* skeleton = clips::skeleton_of(body);
  if (!skeleton) {
    return scales;
  }
  for (std::size_t layer = 0; layer < kLayers; ++layer) {
    const float scale = read<float>(skeleton, kLayerScales + kLayerStride * layer);
    if (std::isfinite(scale) && std::fabs(scale - 1.0f) > kScaleUnchanged) {
      scales.push_back(
          wire::StationLayerScale{static_cast<std::uint8_t>(layer), in_steps(scale, kFineSteps)});
    }
  }
  return scales;
}

// Counts the starts and stops the game made on the grindstone's audio since the last frame.
void count_sounds(void* audio) {
  static std::vector<sounds::Heard> heard;
  sounds::take(audio, heard);
  for (const sounds::Heard& sound : heard) {
    wire::StationSound* counted = nullptr;
    for (wire::StationSound& known : wheel_sounds) {
      if (known.trigger == sound.trigger) {
        counted = &known;
      }
    }
    if (!counted && sound.trigger != 0 && wheel_sounds.size() < wire::kStationEntrySoundsMax) {
      wheel_sounds.push_back(wire::StationSound{sound.trigger, 0, 0});
      counted = &wheel_sounds.back();
    }
    if (!counted) {
      continue;
    }
    if (sound.start) {
      ++counted->starts;
    } else {
      ++counted->stops;
    }
    log_line("GRINDING own sound %08x %s", static_cast<unsigned>(sound.trigger),
             sound.start ? "started" : "stopped");
  }
}

// One of the minigame's spark emitters as an entry, false while the game has none or gave it
// nothing yet.
bool read_sparks(std::size_t offset, const char* key, void* grindstone, wire::StationEntry& entry) {
  void* emitter = read<void*>(minigame, offset);
  if (!emitter) {
    return false;
  }
  emitters::watch(emitter);
  emitters::Given given;
  if (!emitters::given(emitter, given)) {
    return false;
  }
  entry.key = key;
  entry.spawning = true;
  entry.spawn = {std::round(given.count), in_steps(given.strength, kFineSteps)};
  // Where the game placed it, relative to the grindstone: every other client places its own there
  // (the effect attachment's own placement came out turned around on another client, live).
  float place[3];
  float turn[4];
  entity::world_pose(grindstone, place, turn);
  float position[3];
  float rotation[4];
  std::memcpy(position, static_cast<const std::uint8_t*>(emitter) + kEmitterPosition,
              sizeof(position));
  std::memcpy(rotation, static_cast<const std::uint8_t*>(emitter) + kEmitterRotation,
              sizeof(rotation));
  float relative_position[3];
  float relative_rotation[4];
  pose_math::relative(place, turn, position, rotation, relative_position, relative_rotation);
  pose_math::normalize(relative_rotation);
  entry.placed = true;
  for (int axis = 0; axis < 3; ++axis) {
    entry.position[axis] = in_steps(relative_position[axis], kPlaceSteps);
  }
  for (int axis = 0; axis < 4; ++axis) {
    entry.rotation[axis] = in_steps(relative_rotation[axis], kPlaceSteps);
  }
  return true;
}

void clear_owner() {
  published_field.clear();
  published = wire::StationAction{};
  published_at = -1.0;
  watched_grindstone = 0;
  wheel_sounds.clear();
  last_clip.clear();
  last_speed = -1.0f;
  last_scales.clear();
  pedal_clip.clear();
}

// ---------------------------------------------------------------------------------------------
// Every other client's side.

struct Sparks {
  void* emitter = nullptr;
  bool given = false;
  std::array<float, 2> spawn{};
};

struct Shown {
  std::uint32_t grindstone = 0;
  std::array<float, 3> place{};
  std::string clip;
  std::int32_t clip_id = -1;
  bool clip_started = false;
  void* body = nullptr;
  float wheel_time = 0.0f;  // where the wheel stands while the body does not pedal
  std::vector<wire::StationLayerScale> scaled;  // the body's layers this client scaled
  std::vector<wire::StationSound> heard;
  std::vector<std::uint32_t> sounding;
  std::map<std::uint32_t, float> parameters;
  Sparks sparks;
  Sparks fail;
};

std::map<std::uint32_t, Shown> shown;

void* nearest_grindstone(const float place[3]) {
  void* best = nullptr;
  float best_distance = kGrindstoneMatch;
  entity::each_of_class(kGrindstoneClass, [&](void* candidate) {
    float at[3];
    entity::world_position(candidate, at);
    const float x = at[0] - place[0];
    const float y = at[1] - place[1];
    const float z = at[2] - place[2];
    const float distance = std::sqrt(x * x + y * y + z * z);
    if (distance <= best_distance) {
      best = candidate;
      best_distance = distance;
    }
  });
  return best;
}

const wire::StationEntry* entry_of(const wire::StationAction& action, const char* key) {
  for (const wire::StationEntry& entry : action.entries) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

// The replica's body plays its layers at the owner's scales (the pedalling follows the wheel);
// a layer no longer listed, and every layer once the grinding ends, plays at 1 again.
void keep_layers(Shown& one, void* body, const std::vector<wire::StationLayerScale>& scales) {
  void* skeleton = clips::skeleton_of(body);
  if (body != one.body) {
    one.scaled.clear();
    one.body = body;
  }
  if (!skeleton) {
    return;
  }
  auto set = slot_of<SetLayerScale>(skeleton, kLayerPlaybackScale);
  for (const wire::StationLayerScale& old : one.scaled) {
    const bool kept = std::any_of(scales.begin(), scales.end(),
                                  [&](const auto& scale) { return scale.layer == old.layer; });
    if (!kept) {
      set(skeleton, old.layer, 1.0f);
    }
  }
  for (const wire::StationLayerScale& scale : scales) {
    set(skeleton, scale.layer, scale.scale);
  }
  one.scaled = scales;
}

// The wheel plays the owner's wheel clip at the replica's own clip time, as the owner's game keeps
// it at the sharpener's: the replica's pedalling runs at the owner's layer scales, so
// the wheel turns with it and stands still with it. It is started again every frame at that time
// and given one frame of animation with its layer held (this client's game would otherwise run the
// level entity's looping clip by itself and the wheel turned without anyone pedalling, live).
void keep_wheel(std::uint32_t holder, Shown& one, void* grindstone, void* body,
                const std::string& clip, std::uint32_t flags, const std::string& pedalling) {
  void* skeleton = clips::skeleton_of(grindstone);
  if (clip.empty()) {
    if (one.clip_started) {
      clips::stop(skeleton);
      if (skeleton) {
        slot_of<SetLayerScale>(skeleton, kLayerPlaybackScale)(skeleton, 0, 1.0f);
      }
      clips::process(grindstone, kProcessDistance);
      one.clip_started = false;
      log_line("GRINDING holder=%u wheel stopped", holder);
    }
    one.clip.clear();
    one.clip_id = -1;
    return;
  }
  if (clip != one.clip) {
    one.clip = clip;
    one.clip_id = clips::clip_id(clips::character_of(grindstone), clip);
    log_line("GRINDING holder=%u wheel plays %s (%d)", holder, clip.c_str(), one.clip_id);
  }
  if (one.clip_id < 0 || !skeleton) {
    return;
  }
  // Only the pedalling turns the wheel: while the body sits down or stands up the wheel stays where
  // it stood (it followed those clips' time and turned at random, live).
  const std::int32_t pedalling_id =
      pedalling.empty() ? -1 : clips::clip_id(clips::character_of(body), pedalling);
  std::uint32_t body_flags = 0;
  if (body && pedalling_id >= 0 &&
      clips::top_clip(clips::skeleton_of(body), body_flags) == pedalling_id) {
    one.wheel_time = clips::top_clip_time(body);
  }
  clips::clear(skeleton);
  const float time = one.wheel_time;
  const bool started =
      clips::start(skeleton, one.clip_id, flags | clips::kStartAtKeyTime, 0.0f, time);
  if (started) {
    slot_of<SetLayerScale>(skeleton, kLayerPlaybackScale)(skeleton, 0, 0.0f);
    clips::process(grindstone, kProcessDistance);
  }
  if (started != one.clip_started) {
    log_line("GRINDING holder=%u wheel %s at %.2f", holder, started ? "driven" : "refused", time);
  }
  one.clip_started = started;
}

void sound_on(std::uint32_t holder, Shown& one, void* grindstone, std::uint32_t trigger,
              bool start) {
  sounds::play(grindstone, trigger, start);
  log_line("GRINDING holder=%u sound %08x %s", holder, static_cast<unsigned>(trigger),
           start ? "started" : "stopped");
  auto playing = std::find(one.sounding.begin(), one.sounding.end(), trigger);
  if (start && playing == one.sounding.end()) {
    one.sounding.push_back(trigger);
  } else if (!start && playing != one.sounding.end()) {
    one.sounding.erase(playing);
  }
}

// Every start and stop the owner's minigame made on its grindstone since the last field; a sound
// not heard before is started when it was started more often than stopped (the grinding loop a
// late joiner comes into; the counts wrap at 256, so "more often" is a difference of 1 to 127). The
// minigame stops its sound once more as the sharpener leaves, which started it here. Then
// the sound's parameters as the owner's game set them.
void keep_sounds(std::uint32_t holder, Shown& one, void* grindstone,
                 const wire::StationEntry* wheel) {
  if (!wheel) {
    return;
  }
  for (const wire::StationParameter& parameter : wheel->parameters) {
    auto known = one.parameters.find(parameter.id);
    if (known == one.parameters.end() || known->second != parameter.value) {
      sounds::set_parameter(grindstone, parameter.id, parameter.value);
      one.parameters[parameter.id] = parameter.value;
    }
  }
  for (const wire::StationSound& sound : wheel->sounds) {
    const wire::StationSound* before = nullptr;
    for (const wire::StationSound& known : one.heard) {
      if (known.trigger == sound.trigger) {
        before = &known;
      }
    }
    if (!before) {
      const int playing_now = static_cast<std::uint8_t>(sound.starts - sound.stops);
      if (playing_now > 0 && playing_now < 128) {
        sound_on(holder, one, grindstone, sound.trigger, true);
      }
      continue;
    }
    const int stops = static_cast<std::uint8_t>(sound.stops - before->stops);
    const int starts = static_cast<std::uint8_t>(sound.starts - before->starts);
    const bool playing =
        std::find(one.sounding.begin(), one.sounding.end(), sound.trigger) != one.sounding.end();
    if (stops > 0 && playing) {
      sound_on(holder, one, grindstone, sound.trigger, false);
    }
    if (starts > 0) {
      sound_on(holder, one, grindstone, sound.trigger, true);
    }
  }
  one.heard = wheel->sounds;
}

// Removes a spark emitter this client made at its grindstone, and its attachment.
void remove_sparks(Sparks& sparks) {
  if (!sparks.emitter) {
    return;
  }
  emitters::give_emitter(sparks.emitter, emitters::Given{0.0f, 0.0f});
  void* manager = natives.particle_manager ? *natives.particle_manager : nullptr;
  if (manager) {
    slot_of<DeleteEmitter>(manager, kDeleteEmitter)(manager, sparks.emitter);
  }
  sparks = Sparks{};
}

// A spark emitter with the owner's spawn parameters where the owner's game placed its own; without
// the entry it throws nothing. It is spawned free in the world, as the effect attachment spawns its
// emitter: this client's game moved one made at the grindstone's attachment back onto the joint
// every frame, while the owner's minigame turns its own away from it (the sparks flew the wrong
// way, live). An owner that reports no place gives the joint's.
void keep_sparks(std::uint32_t holder, Sparks& sparks, void* grindstone, const char* effect,
                 const wire::StationEntry* entry) {
  const std::array<float, 2> spawn =
      entry && entry->spawning ? entry->spawn : std::array<float, 2>{0.0f, 0.0f};
  Location location{};
  location.scale = 1.0f;
  bool placed = false;
  if (entry && entry->placed) {
    float place[3];
    float turn[4];
    entity::world_pose(grindstone, place, turn);
    pose_math::compose(place, turn, entry->position.data(), entry->rotation.data(),
                       location.position, location.rotation);
    placed = true;
  } else {
    placed = entity::joint_pose(grindstone, kSparksJoint, location.position, location.rotation);
  }
  if (!sparks.emitter) {
    void* manager = natives.particle_manager ? *natives.particle_manager : nullptr;
    if (spawn[0] <= 0.0f || !placed || !manager) {
      return;
    }
    void* found = slot_of<FindEffect>(manager, kFindEffect)(manager, effect, "", true);
    sparks.emitter =
        found ? slot_of<SpawnEmitter>(found, kSpawnEmitter)(found, &location, 0) : nullptr;
    log_line("GRINDING holder=%u sparks %s %s", holder, effect,
             sparks.emitter ? "made" : "not made");
    if (!sparks.emitter) {
      return;
    }
  }
  if (!sparks.given || sparks.spawn != spawn) {
    emitters::give_emitter(sparks.emitter, emitters::Given{spawn[0], spawn[1]});
    sparks.spawn = spawn;
    sparks.given = true;
  }
  if (placed) {
    slot_of<SetLocation>(sparks.emitter, kSetLocation)(sparks.emitter, &location);
  }
}

// Everything this client showed for a replica goes as the level had it.
void release(std::uint32_t holder, Shown& one) {
  void* grindstone = entity::by_id(one.grindstone);
  if (grindstone) {
    keep_wheel(holder, one, grindstone, nullptr, std::string(), 0, std::string());
    const std::vector<std::uint32_t> playing = one.sounding;
    for (const std::uint32_t trigger : playing) {
      sound_on(holder, one, grindstone, trigger, false);
    }
    remove_sparks(one.sparks);
    remove_sparks(one.fail);
  }
  keep_layers(one, one.body, {});
  log_line("GRINDING holder=%u grinding ended", holder);
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  const std::size_t at =
      kcd2s::bridge::rtti_vtable(natives.image, natives.image_size,
                                 reinterpret_cast<std::uintptr_t>(natives.image), kMinigameType);
  if (at == kcd2s::bridge::knpos) {
    log_line("GRINDING the minigame's vtable is missing");
    return false;
  }
  void* target = read<void*>(natives.image, at + kUpdateSlot * sizeof(void*));
  if (!target ||
      MH_CreateHook(target, reinterpret_cast<void*>(&hooked_update),
                    reinterpret_cast<void**>(&original_update)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    log_line("GRINDING could not hook the minigame's update %p", target);
    return false;
  }
  log_line("GRINDING minigame update %p hooked, particles %p", target, natives.particle_manager);
  return true;
}

bool own_running() {
  return grinding_now();
}

std::string own_field(void* actor, void* body, double now) {
  local_actor = actor;
  if (!actor || !body || !grinding_now()) {
    if (!published_field.empty()) {
      log_line("GRINDING own action ended");
    }
    minigame = nullptr;
    clear_owner();
    return {};
  }
  const std::uint32_t grindstone_id = read<std::uint32_t>(minigame, kGrindstone);
  void* grindstone = entity::by_id(grindstone_id);
  if (!grindstone) {
    return published_field;
  }
  void* audio = sounds::audio_of(grindstone);
  if (grindstone_id != watched_grindstone) {
    watched_grindstone = grindstone_id;
    sounds::watch(audio);
    log_line("GRINDING own grindstone %u audio %p", grindstone_id, audio);
  }
  count_sounds(audio);

  wire::StationAction action;
  action.kind = wire::kStationKindGrind;
  float place[3];
  entity::world_position(grindstone, place);
  action.station = {place[0], place[1], place[2]};
  std::uint32_t flags = 0;
  const std::int32_t clip = clips::top_clip(clips::skeleton_of(grindstone), flags);
  const std::string clip_name = clips::clip_name(clips::character_of(grindstone), clip);
  if (wire::station_action_detail::clip_name_valid(clip_name)) {
    action.bellows_clip = clip_name;
    action.bellows_flags = flags;
  }
  const float speed = read<float>(minigame, kWheelSpeed);
  action.turning = true;
  action.wheel_speed = std::isfinite(speed) ? in_steps((std::max)(speed, 0.0f), kFineSteps) : 0.0f;
  action.layer_scales = layer_scales_of(body);
  if (speed > 0.0f) {
    std::uint32_t body_flags = 0;
    const std::int32_t pedalling = clips::top_clip(clips::skeleton_of(body), body_flags);
    const std::string seen = pedalling >= 0 ? "#" + std::to_string(pedalling) : std::string();
    if (!seen.empty() && seen != pedal_clip) {
      pedal_clip = seen;
      log_line("GRINDING own pedalling clip %s", pedal_clip.c_str());
    }
  }
  action.body_clip = pedal_clip;
  if (action.bellows_clip != last_clip || (speed > 0.0f) != (last_speed > 0.0f) ||
      !same_layers(action.layer_scales, last_scales)) {
    std::string layers;
    for (const wire::StationLayerScale& scale : action.layer_scales) {
      char text[32];
      std::snprintf(text, sizeof(text), " %u:%.2f", unsigned(scale.layer), scale.scale);
      layers += text;
    }
    log_line("GRINDING own wheel clip %s flags %u speed %.2f layers%s",
             action.bellows_clip.empty() ? "-" : action.bellows_clip.c_str(), action.bellows_flags,
             speed, layers.empty() ? " -" : layers.c_str());
    last_clip = action.bellows_clip;
    last_speed = speed;
    last_scales = action.layer_scales;
  }

  wire::StationEntry wheel;
  wheel.key = wire::kStationGrindWheel;
  wheel.sounds = wheel_sounds;
  static std::vector<sounds::Parameter> parameters;
  sounds::parameters(audio, parameters);
  for (const sounds::Parameter& parameter : parameters) {
    if (parameter.id != 0 && wheel.parameters.size() < wire::kStationEntryParametersMax) {
      wheel.parameters.push_back(
          wire::StationParameter{parameter.id, in_steps(parameter.value, kFineSteps)});
    }
  }
  action.entries.push_back(wheel);
  wire::StationEntry sparks;
  if (read_sparks(kSparksEmitter, wire::kStationGrindSparks, grindstone, sparks)) {
    action.entries.push_back(sparks);
  }
  wire::StationEntry fail;
  if (read_sparks(kFailEmitter, wire::kStationGrindFail, grindstone, fail)) {
    action.entries.push_back(fail);
  }

  // The clip and the sounds go at once; the sparks and the parameters at most every
  // kPublishInterval.
  const bool sudden = published.bellows_clip != action.bellows_clip ||
                      (published.wheel_speed > 0.0f) != (action.wheel_speed > 0.0f) ||
                      published.layer_scales.size() != action.layer_scales.size() ||
                      published.entries.empty() ||
                      published.entries[0].sounds != action.entries[0].sounds;
  if (action == published || (!sudden && now - published_at < kPublishInterval)) {
    return published_field;
  }
  const std::string field = wire::station_action_field(action);
  if (field.empty()) {
    return published_field;
  }
  if (published_field.empty()) {
    log_line("GRINDING own action %s", field.c_str());
  }
  published = action;
  published_field = field;
  published_at = now;
  return published_field;
}

void keep(std::uint32_t holder, void* body, const std::string& field, double now) {
  (void)now;
  wire::StationAction action;
  const bool grinding = !field.empty() && wire::parse_station_action_field(field, action) &&
                        action.kind == wire::kStationKindGrind;
  auto it = shown.find(holder);
  if (!grinding) {
    if (it != shown.end()) {
      release(holder, it->second);
      shown.erase(it);
    }
    return;
  }
  if (it == shown.end()) {
    it = shown.emplace(holder, Shown{}).first;
  }
  Shown& one = it->second;
  if (one.grindstone == 0 || one.place != action.station) {
    if (one.grindstone != 0) {
      release(holder, one);
      one = Shown{};
    }
    void* found = nearest_grindstone(action.station.data());
    if (!found) {
      return;
    }
    one.grindstone = entity::id_of(found);
    one.place = action.station;
    log_line("GRINDING holder=%u grinds at grindstone %u", holder, one.grindstone);
  }
  void* grindstone = entity::by_id(one.grindstone);
  if (!grindstone) {
    return;
  }
  keep_layers(one, body, action.layer_scales);
  keep_wheel(holder, one, grindstone, body, action.bellows_clip, action.bellows_flags,
             action.body_clip);
  keep_sounds(holder, one, grindstone, entry_of(action, wire::kStationGrindWheel));
  keep_sparks(holder, one.sparks, grindstone, kSparksEffect,
              entry_of(action, wire::kStationGrindSparks));
  keep_sparks(holder, one.fail, grindstone, kFailEffect, entry_of(action, wire::kStationGrindFail));
}

void forget(std::uint32_t holder) {
  auto it = shown.find(holder);
  if (it == shown.end()) {
    return;
  }
  release(holder, it->second);
  shown.erase(it);
}

void reset() {
  shown.clear();
  minigame = nullptr;
  clear_owner();
}

}  // namespace kcds::runtime::crafting::grinding
