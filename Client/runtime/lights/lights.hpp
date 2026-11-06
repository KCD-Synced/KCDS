// Point lights: the server's Light.spawn (LIGHT orders, kcd2s/
// integration/lights.hpp) and a client script's own (Client/resources/lights.cjs), shown the same
// way, WHGame 1.5.5:
//
//   { op: 'lightLocal', kind: 'spawn' | 'update', id, position, color, intensity, radius,
//     castShadows }
//   { op: 'lightLocal', kind: 'destroy', id }
//
// A light is a BasicEntity of its own (its default model taken out, as the placed effects' carriers
// are) with a CDLight in one of its slots, as the entity script binding LoadLight puts one there
// (ScriptBind_Entity): the CDLight is made by its constructor (800 bytes) and
// filled as that binding fills it from a light table: a point light (flags + 12 |=
// 0x40, and 0x30000020 for shadow maps), the radius at + 516 and as its attenuation radius (+ 28,
// + 204) with the constructor's bulb of 0.1 m (+ 528; with none the lit surfaces turned black,
// live), and the colour times the intensity at + 32 and + 496. Then the entity loads it
// (the entity's render component, made when it has none, takes the light into the slot). A change
// loads the light into the same slot again; a move places the entity.
//
// A script's lights carry ids from kLocalLightIds up, which the server never gives. At most
// kMaxLights show at once on this client, the server's and the scripts' together.
//
// Engine thread only, except handle_runtime_message and pump.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "kcd2s/integration/lights.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::lights {

inline constexpr std::uint32_t kLocalLightIds = 0xF0000000u;
inline constexpr std::size_t kMaxLights = 192;

struct Natives {
  void* make_light = nullptr;  // CDLight::CDLight
  void* load_light = nullptr;  // the entity takes a light into a slot
  void (*log)(const char* format, ...) = nullptr;
  // A light on a body: the body's entity by its network id, and AttachChild (as the
  // bursts on bodies use them, vfx/placed_effects.hpp).
  void* (*body_of)(std::uint32_t network_id) = nullptr;
  void* attach_child = nullptr;
};

// False while a native is missing; no light shows then.
bool install(const Natives& natives);

// A script's lightLocal op (any thread): true when the message was one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Shows, changes or removes a light (engine thread).
void apply(const kcd2s::integration::LightOrder& order);

// The scripts' orders since the last frame (engine thread).
void tick();

// The world went: its lights went with it. The server sends its own again; the scripts hear that
// theirs are gone.
void reset();

// Tells the client resources which of their lights this client no longer shows (a world change,
// or no room for it): { op: 'lightsGone', ids } (network thread, network_mutex held).
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
void pump(const RuntimeSender& send);

}  // namespace kcds::runtime::lights
