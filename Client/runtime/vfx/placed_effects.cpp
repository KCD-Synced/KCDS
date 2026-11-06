#include "placed_effects.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../props/prop_entity.hpp"

namespace kcds::runtime::vfx::placed {
namespace {

using kcd2s::integration::VfxKind;
using kcd2s::integration::VfxOrder;
using kcd2s::integration::VfxTuning;

// The particle manager: FindEffect(name, requester, load resources) is at byte offset 48.
constexpr std::size_t kFindEffectSlot = 6;
// The emitter's spawn parameters (built, 65 bytes): the tuning values.
constexpr std::size_t kSpawnParamsSize = 80;
constexpr std::size_t kCountScale = 8;
constexpr std::size_t kScale = 12;
constexpr std::size_t kSpeedScale = 16;
constexpr std::size_t kTimeScale = 20;
constexpr std::size_t kPulsePeriod = 24;
constexpr std::size_t kStrength = 28;
// A new slot of the entity for the emitter.
constexpr int kNewSlot = -1;
constexpr char kCarrierClass[] = "BasicEntity";
// CEntity::FreeSlot's vtable slot, and the slot the BasicEntity script loads its model into.
constexpr std::size_t kFreeSlotSlot = 87;
constexpr int kModelSlot = 0;
constexpr char kRequester[] = "kcds_vfx";

// SChildAttachParams: no flags attaches to the parent entity itself, flag 4 to a joint of its
// character by name (as a held prop hangs, animation_props.cpp).
struct AttachParams {
  std::int32_t flags;
  const char* bone;
};
constexpr std::int32_t kAttachToJoint = 4;

using FindEffect = void*(__fastcall*)(void* manager, const char* name, const char* requester,
                                      bool load_resources);
using SpawnParamsInit = void*(__fastcall*)(void* params);
using LoadEmitter = int(__fastcall*)(void* entity, int slot, void* effect, const void* params,
                                     bool prime, bool serialize);
using AttachChild = bool(__fastcall*)(void* parent, void* child, const AttachParams* params);
using FreeSlot = void(__fastcall*)(void* entity, int slot);

Natives natives;

struct Effect {
  std::string name;
  float position[3]{};
  float rotation[4]{};
  VfxTuning tuning;
  bool prime = false;
  // The scale the emitter was loaded with; a later scale is its entity's.
  float built_scale = 1.0f;
  // The body it hangs on (a network id, 0 for none), its joint, and the body's entity it was hung
  // on (null while it waits for the body).
  std::uint32_t body = 0;
  std::string joint;
  void* parent = nullptr;
};

struct Burst {
  std::string entity_name;
  ULONGLONG ends_at = 0;
};

// Engine thread only.
std::unordered_map<std::uint32_t, Effect> effects;
std::vector<Burst> bursts;
std::uint32_t burst_counter = 0;
// Effects on bodies are looked at this often (ms): a body that came, or one that was rebuilt.
constexpr ULONGLONG kBodyCheckMs = 500;
ULONGLONG next_body_check = 0;

std::string effect_entity(std::uint32_t id) {
  return "kcds_vfx_" + std::to_string(id);
}

void set_float(std::uint8_t* params, std::size_t offset, float value) {
  std::memcpy(params + offset, &value, sizeof(value));
}

// Loads the effect into a new slot of the entity; false when the library has no such effect.
bool load(void* entity, const std::string& effect, const VfxTuning& tuning, bool prime) {
  void* manager = natives.particle_manager ? *natives.particle_manager : nullptr;
  if (!entity || !manager) {
    return false;
  }
  void** table = *static_cast<void***>(manager);
  void* found = reinterpret_cast<FindEffect>(table[kFindEffectSlot])(manager, effect.c_str(),
                                                                     kRequester, true);
  if (!found) {
    return false;
  }
  alignas(16) std::uint8_t params[kSpawnParamsSize];
  reinterpret_cast<SpawnParamsInit>(natives.spawn_params_init)(params);
  set_float(params, kScale, tuning.scale);
  set_float(params, kCountScale, tuning.count_scale);
  set_float(params, kSpeedScale, tuning.speed_scale);
  set_float(params, kTimeScale, tuning.time_scale);
  set_float(params, kPulsePeriod, tuning.pulse_period);
  set_float(params, kStrength, tuning.strength);
  return reinterpret_cast<LoadEmitter>(natives.load_emitter)(entity, kNewSlot, found, params, prime,
                                                             false) >= 0;
}

// A BasicEntity to carry an emitter. Its script loads the class's default model (a white
// pyramid) into slot 0 when it spawns; CEntity::FreeSlot (vtable slot 87) takes it out.
// The body the script gave it goes too: a burst's carrier hung on a player kept pushing the player
// up (in the air every 0.3 s while it played, then the game's fall damage).
void* spawn_carrier(const std::string& name, const float position[3]) {
  void* carrier = props::entity::spawn_class(name, kCarrierClass, position);
  if (carrier) {
    void** table = *static_cast<void***>(carrier);
    reinterpret_cast<FreeSlot>(table[kFreeSlotSlot])(carrier, kModelSlot);
    props::entity::physicalize(carrier, props::entity::Physics::None, 0.0f);
  }
  return carrier;
}

// Hangs a carrier on a body, or on its joint when the body has one of that name; true when it
// went on the joint. A joint the body does not have hangs it on the body itself: the game's attach
// takes any name (it said attached and the effect played nowhere), so the joint is looked up
// first.
bool hang(void* body, void* carrier, const std::string& joint) {
  const auto attach_child = reinterpret_cast<AttachChild>(natives.attach_child);
  float joint_position[3];
  float joint_rotation[4];
  if (!joint.empty() &&
      props::entity::joint_pose(body, joint.c_str(), joint_position, joint_rotation)) {
    const AttachParams on_joint{kAttachToJoint, joint.c_str()};
    if (attach_child(body, carrier, &on_joint)) {
      return true;
    }
  }
  const AttachParams on_body{0, nullptr};
  attach_child(body, carrier, &on_body);
  return false;
}

// Removes the effect's entity when it has one, then builds it from what the effect shows; one on
// a body hangs on it. False when it could not be built (or its body is not here).
bool build(std::uint32_t id, Effect& effect) {
  const std::string name = effect_entity(id);
  props::entity::remove(name);
  effect.parent = nullptr;
  void* body = nullptr;
  if (effect.body != 0) {
    body = natives.body_of ? natives.body_of(effect.body) : nullptr;
    if (!body || !natives.attach_child) {
      return false;
    }
  }
  void* carrier = spawn_carrier(name, effect.position);
  if (!carrier) {
    return false;
  }
  if (body) {
    const bool on_joint = hang(body, carrier, effect.joint);
    props::entity::place_rotated(carrier, effect.position, effect.rotation);
    effect.parent = body;
    if (natives.log) {
      natives.log("VFX %u %s on body %u%s%s", id, effect.name.c_str(), effect.body,
                  on_joint ? ", joint " : "", on_joint ? effect.joint.c_str() : "");
    }
  } else {
    props::entity::rotate(carrier, effect.rotation);
  }
  if (!load(carrier, effect.name, effect.tuning, effect.prime)) {
    props::entity::remove(name);
    effect.parent = nullptr;
    return false;
  }
  return true;
}

Effect effect_of(const VfxOrder& order) {
  Effect effect;
  effect.name = order.effect;
  for (std::size_t i = 0; i < 3; ++i) {
    effect.position[i] = order.position[i];
  }
  for (std::size_t i = 0; i < 4; ++i) {
    effect.rotation[i] = order.rotation[i];
  }
  effect.tuning = order.tuning;
  effect.prime = order.prime;
  if (order.kind == VfxKind::Spawn) {
    effect.body = order.body;
    effect.joint = order.joint;
  }
  return effect;
}

void spawn(const VfxOrder& order) {
  Effect effect = effect_of(order);
  effect.built_scale = effect.tuning.scale;
  const bool built = build(order.id, effect);
  if (natives.log) {
    natives.log("VFX %u %s: %s", order.id, order.effect.c_str(),
                built ? "spawned" : (effect.body != 0 ? "waits for its body" : "not spawned"));
  }
  // One on a body that is not here yet comes when it does.
  if (built || effect.body != 0) {
    effects[order.id] = effect;
  }
}

// Whether the two tunings differ in their scale only.
bool only_scale_differs(const VfxTuning& before, const VfxTuning& after) {
  VfxTuning same = after;
  same.scale = before.scale;
  return same == before && after.scale != before.scale;
}

void update(const VfxOrder& order) {
  const auto found = effects.find(order.id);
  if (found == effects.end()) {
    return;
  }
  Effect& effect = found->second;
  // A new scale grows or shrinks the running emitter through its entity.
  if (only_scale_differs(effect.tuning, order.tuning) && effect.built_scale > 0.0f) {
    void* carrier = props::entity::find(effect_entity(order.id));
    if (carrier) {
      const float factor = order.tuning.scale / effect.built_scale;
      const float scale[3] = {factor, factor, factor};
      props::entity::set_scale(carrier, scale);
      effect.tuning = order.tuning;
      return;
    }
  }
  effect.tuning = order.tuning;
  effect.built_scale = order.tuning.scale;
  if (!build(order.id, effect) && effect.body == 0) {
    effects.erase(found);
  }
}

void destroy(const VfxOrder& order) {
  props::entity::remove(effect_entity(order.id));
  effects.erase(order.id);
}

// Plays the burst on an entity of its own, removed when its duration is over.
void burst(const VfxOrder& order) {
  const Effect effect = effect_of(order);
  const std::string name = "kcds_burst_" + std::to_string(++burst_counter);
  void* carrier = spawn_carrier(name, effect.position);
  if (!carrier) {
    if (natives.log) {
      natives.log("VFX burst %s: no carrier entity", effect.name.c_str());
    }
    return;
  }
  props::entity::rotate(carrier, effect.rotation);
  if (!load(carrier, effect.name, effect.tuning, effect.prime)) {
    if (natives.log) {
      natives.log("VFX burst %s: the game did not load the effect", effect.name.c_str());
    }
    props::entity::remove(name);
    return;
  }
  bursts.push_back({name, GetTickCount64() + order.duration_ms});
}

// Plays the burst on an entity attached to a body, or to one of its joints, which
// it follows at the order's offset.
void burst_on_body(const VfxOrder& order) {
  void* body = natives.body_of ? natives.body_of(order.id) : nullptr;
  if (!body || !natives.attach_child) {
    return;
  }
  const Effect effect = effect_of(order);
  const std::string name = "kcds_burst_" + std::to_string(++burst_counter);
  void* carrier = spawn_carrier(name, effect.position);
  if (!carrier) {
    return;
  }
  const bool attached = hang(body, carrier, order.joint);
  if (natives.log && !order.joint.empty()) {
    natives.log("VFX burst %s on joint %s: %s", order.effect.c_str(), order.joint.c_str(),
                attached ? "attached" : "not found, on the body");
  }
  props::entity::place_rotated(carrier, effect.position, effect.rotation);
  if (!load(carrier, effect.name, effect.tuning, effect.prime)) {
    props::entity::remove(name);
    return;
  }
  bursts.push_back({name, GetTickCount64() + order.duration_ms});
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  return natives.particle_manager && natives.spawn_params_init && natives.load_emitter;
}

void apply(const VfxOrder& order) {
  switch (order.kind) {
    case VfxKind::Spawn:
      spawn(order);
      break;
    case VfxKind::Update:
      update(order);
      break;
    case VfxKind::Destroy:
      destroy(order);
      break;
    case VfxKind::Burst:
      burst(order);
      break;
    case VfxKind::BurstOn:
      burst_on_body(order);
      break;
  }
}

void tick() {
  const ULONGLONG now = GetTickCount64();
  // Effects on bodies: one whose body came is built, one whose body went or was
  // rebuilt (a respawn, a new replica) is built again on the body there is now.
  if (now >= next_body_check) {
    next_body_check = now + kBodyCheckMs;
    for (auto& [id, effect] : effects) {
      if (effect.body == 0) {
        continue;
      }
      void* body = natives.body_of ? natives.body_of(effect.body) : nullptr;
      if (body != effect.parent && body) {
        build(id, effect);
      }
    }
  }
  if (bursts.empty()) {
    return;
  }
  std::vector<Burst> running;
  for (const Burst& playing : bursts) {
    if (playing.ends_at <= now) {
      props::entity::remove(playing.entity_name);
    } else {
      running.push_back(playing);
    }
  }
  bursts.swap(running);
}

void reset() {
  effects.clear();
  bursts.clear();
  next_body_check = 0;
}

}  // namespace kcds::runtime::vfx::placed
