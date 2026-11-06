#include "lights.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kcd2s/integration/script_channel.hpp"

#include "../props/prop_entity.hpp"

namespace kcds::runtime::lights {

namespace {

namespace integration = kcd2s::integration;
namespace sdk = kcd2s::sdk;

// CDLight (WHGame 1.5.5): 800 bytes, made by its constructor.
constexpr std::size_t kLightSize = 800;
constexpr std::size_t kFlags = 12;
constexpr std::size_t kAttenuationRadius = 28;  // and + 204, the radius the falloff ends at
constexpr std::size_t kAttenuationRadiusCopy = 204;
constexpr std::size_t kColor = 32;  // ColorF, and again at + 496
constexpr std::size_t kColorCopy = 496;
constexpr std::size_t kBoxSize = 88;  // half sizes x, y, z: the radius, as LoadLight
constexpr std::size_t kRadius = 516;
constexpr std::uint32_t kPointLight = 0x40;
constexpr std::uint32_t kShadowMaps = 0x30000020;
// The slot of the light in its entity (the carrier's model slot 0 is taken out).
constexpr int kLightSlot = 1;

// The carrier: a BasicEntity whose script loads the class's default model into slot 0 when it
// spawns; CEntity::FreeSlot (vtable slot 87) takes it out, as the placed effects do.
constexpr char kCarrierClass[] = "BasicEntity";
constexpr std::size_t kFreeSlotSlot = 87;
constexpr int kModelSlot = 0;

using MakeLight = void*(__fastcall*)(void* light);
using LoadLight = int(__fastcall*)(void* entity, int slot, void* light);
using FreeSlot = void(__fastcall*)(void* entity, int slot);

// The scripts' orders a frame may hold (more are dropped), and the ids of their lights that went.
constexpr std::size_t kMaxQueued = 256;

struct Shown {
  std::string name;
  integration::LightOrder order;
  // The body entity the light hangs on, null for a light in the world.
  void* parent = nullptr;
};

// SChildAttachParams: no flags attaches to the parent entity itself, flag 4 to a joint of its
// character by name (as the bursts on bodies, vfx/placed_effects.cpp).
struct AttachParams {
  std::int32_t flags;
  const char* bone;
};
constexpr std::int32_t kAttachToJoint = 4;
using AttachChild = bool(__fastcall*)(void* parent, void* child, const AttachParams* params);
// A light on a body that is not in this game yet (or got rebuilt) is tried again this often.
constexpr ULONGLONG kBodyRetryMs = 500;

Natives natives;
bool ready = false;
std::unordered_map<std::uint32_t, Shown> shown;
unsigned generation = 0;

// Lights on bodies this game does not show (yet): tried again by tick().
std::unordered_map<std::uint32_t, integration::LightOrder> waiting;
ULONGLONG next_body_check = 0;

std::mutex queue_mutex;
std::vector<integration::LightOrder> queued;
std::vector<std::uint32_t> gone;  // the scripts' lights this client no longer shows

void say(const char* format, std::uint32_t id) {
  if (natives.log) {
    natives.log(format, static_cast<unsigned>(id));
  }
}

template <class T>
void write(std::uint8_t* light, std::size_t offset, T value) {
  std::memcpy(light + offset, &value, sizeof(value));
}

template <class T>
T read(const std::uint8_t* light, std::size_t offset) {
  T value{};
  std::memcpy(&value, light + offset, sizeof(value));
  return value;
}

bool local_id(std::uint32_t id) {
  return id >= kLocalLightIds;
}

// A light the game is told it lost, when a script made it.
void lost(std::uint32_t id) {
  if (!local_id(id)) {
    return;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  gone.push_back(id);
}

// The light into the carrier's slot, as LoadLight does from a script's light table.
bool load(void* carrier, const integration::LightOrder& order) {
  alignas(16) std::uint8_t light[kLightSize] = {};
  reinterpret_cast<MakeLight>(natives.make_light)(light);
  std::uint32_t flags = read<std::uint32_t>(light, kFlags) | kPointLight;
  if (order.cast_shadows) {
    flags |= kShadowMaps;
  }
  write(light, kFlags, flags);
  // The constructor's bulb (0.1 m) stays: with none the game's falloff divides by zero and the
  // lit surfaces turn black, speckled.
  write(light, kRadius, order.radius);
  write(light, kAttenuationRadius, order.radius);
  write(light, kAttenuationRadiusCopy, order.radius);
  for (std::size_t axis = 0; axis < 3; ++axis) {
    write(light, kBoxSize + axis * sizeof(float), order.radius);
  }
  const std::array<float, 4> color{order.color[0] * order.intensity,
                                   order.color[1] * order.intensity,
                                   order.color[2] * order.intensity, 1.0f};
  std::memcpy(light + kColor, color.data(), sizeof(color));
  std::memcpy(light + kColorCopy, color.data(), sizeof(color));
  return reinterpret_cast<LoadLight>(natives.load_light)(carrier, kLightSlot, light) >= 0;
}

// The physics body the BasicEntity's script gives it goes too: hung on a player it pushed the
// player up, as the effects' carriers did.
void* spawn_carrier(const std::string& name, const float position[3]) {
  void* carrier = props::entity::spawn_class(name, kCarrierClass, position);
  if (carrier) {
    void** table = *static_cast<void***>(carrier);
    reinterpret_cast<FreeSlot>(table[kFreeSlotSlot])(carrier, kModelSlot);
    props::entity::physicalize(carrier, props::entity::Physics::None, 0.0f);
  }
  return carrier;
}

// Hangs the carrier on the body (or its joint when the body has one of that name) at the order's
// offset; false when this game has no such body.
bool hang(void* carrier, const integration::LightOrder& order, void*& parent) {
  parent = natives.body_of ? natives.body_of(order.body) : nullptr;
  if (!parent || !natives.attach_child) {
    return false;
  }
  const auto attach_child = reinterpret_cast<AttachChild>(natives.attach_child);
  float joint_position[3];
  float joint_rotation[4];
  const bool on_joint =
      !order.joint.empty() &&
      props::entity::joint_pose(parent, order.joint.c_str(), joint_position, joint_rotation);
  const AttachParams params{on_joint ? kAttachToJoint : 0,
                            on_joint ? order.joint.c_str() : nullptr};
  attach_child(parent, carrier, &params);
  const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  props::entity::place_rotated(carrier, order.position.data(), identity);
  return true;
}

void remove(std::uint32_t id) {
  waiting.erase(id);
  const auto found = shown.find(id);
  if (found == shown.end()) {
    return;
  }
  props::entity::remove(found->second.name);
  shown.erase(found);
}

void show(const integration::LightOrder& order) {
  remove(order.id);
  if (shown.size() >= kMaxLights) {
    say("LIGHTS light %u not shown: this client shows the most it can", order.id);
    lost(order.id);
    return;
  }
  waiting.erase(order.id);
  if (order.body != 0 && !(natives.body_of && natives.body_of(order.body))) {
    // The body is not in this game (yet): the light comes when it does.
    waiting[order.id] = order;
    return;
  }
  char name[48];
  std::snprintf(name, sizeof(name), "kcds_light_%u_%u", order.id, ++generation);
  void* carrier = spawn_carrier(name, order.position.data());
  void* parent = nullptr;
  if (!carrier || (order.body != 0 && !hang(carrier, order, parent)) || !load(carrier, order)) {
    say("LIGHTS light %u could not be made", order.id);
    if (carrier) {
      props::entity::remove(name);
    }
    lost(order.id);
    return;
  }
  shown[order.id] = Shown{name, order, parent};
  say("LIGHTS light %u shown", order.id);
}

void change(const integration::LightOrder& order) {
  const auto found = shown.find(order.id);
  if (found == shown.end()) {
    show(order);
    return;
  }
  void* carrier = props::entity::find(found->second.name);
  if (!carrier || order.body != 0 || found->second.order.body != 0) {
    // A light on a body is hung again as a whole.
    show(order);
    return;
  }
  if (order.position != found->second.order.position) {
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    props::entity::place_in_world(carrier, order.position.data(), identity);
  }
  load(carrier, order);
  found->second.order = order;
}

double number(const sdk::Value& message, const char* name, double fallback) {
  const sdk::Value* value = integration::script_field(message, name);
  if (!value) {
    return fallback;
  }
  if (value->tag() == sdk::Tag::Double) {
    return value->as_double();
  }
  if (value->tag() == sdk::Tag::Int) {
    return double(value->as_int());
  }
  return fallback;
}

std::array<float, 3> triple(const sdk::Value& message, const char* name) {
  std::array<float, 3> out{};
  const sdk::Value* value = integration::script_field(message, name);
  if (value && value->tag() == sdk::Tag::Map) {
    out[0] = float(number(*value, "x", 0));
    out[1] = float(number(*value, "y", 0));
    out[2] = float(number(*value, "z", 0));
  }
  return out;
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  ready = natives.make_light && natives.load_light;
  return ready;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "lightLocal") {
    return false;
  }
  const sdk::Value* kind = integration::script_field(message, "kind");
  const std::string kind_text =
      kind && kind->tag() == sdk::Tag::Str ? kind->as_str() : std::string();
  integration::LightOrder order;
  order.id = std::uint32_t(number(message, "id", 0));
  if (kind_text == "spawn" || kind_text == "update") {
    order.kind =
        kind_text == "spawn" ? integration::LightKind::Spawn : integration::LightKind::Update;
    order.position = triple(message, "position");
    order.color = triple(message, "color");
    order.intensity = float(number(message, "intensity", 1));
    order.radius = float(number(message, "radius", 5));
    const sdk::Value* shadows = integration::script_field(message, "castShadows");
    order.cast_shadows = shadows && shadows->tag() == sdk::Tag::Bool && shadows->as_bool();
    const double body = number(message, "body", 0);
    order.body = body >= 1 && body <= 4294967295.0 ? std::uint32_t(body) : 0;
    const sdk::Value* joint = integration::script_field(message, "joint");
    order.joint = joint && joint->tag() == sdk::Tag::Str ? joint->as_str() : std::string();
  } else if (kind_text == "destroy") {
    order.kind = integration::LightKind::Destroy;
  } else {
    return true;
  }
  if (!local_id(order.id) || !integration::light_valid(order)) {
    return true;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(order);
  }
  return true;
}

void apply(const integration::LightOrder& order) {
  if (!ready) {
    return;
  }
  switch (order.kind) {
    case integration::LightKind::Spawn:
      show(order);
      break;
    case integration::LightKind::Update:
      change(order);
      break;
    case integration::LightKind::Destroy:
      remove(order.id);
      break;
  }
}

void tick() {
  std::vector<integration::LightOrder> orders;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    orders.swap(queued);
  }
  for (const auto& order : orders) {
    apply(order);
  }
  // Lights on bodies: one whose body came is shown, one whose body went or was rebuilt (a replica
  // made again) is hung again.
  const ULONGLONG now = GetTickCount64();
  if (now < next_body_check) {
    return;
  }
  next_body_check = now + kBodyRetryMs;
  std::vector<integration::LightOrder> again;
  for (const auto& [id, order] : waiting) {
    if (natives.body_of && natives.body_of(order.body)) {
      again.push_back(order);
    }
  }
  for (const auto& [id, light] : shown) {
    if (light.order.body != 0 &&
        (!natives.body_of || natives.body_of(light.order.body) != light.parent)) {
      again.push_back(light.order);
    }
  }
  for (const auto& order : again) {
    show(order);
  }
}

void reset() {
  for (const auto& [id, light] : shown) {
    lost(id);
  }
  shown.clear();
  waiting.clear();
}

void pump(const RuntimeSender& send) {
  std::vector<std::uint32_t> ids;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    ids.swap(gone);
  }
  if (ids.empty()) {
    return;
  }
  sdk::Value::Array list;
  for (const std::uint32_t id : ids) {
    list.push_back(sdk::Value(std::int64_t(id)));
  }
  sdk::Value::Map message{{"op", sdk::Value(std::string("lightsGone"))},
                          {"ids", sdk::Value(std::move(list))}};
  send(sdk::Value(std::move(message)));
}

}  // namespace kcds::runtime::lights
