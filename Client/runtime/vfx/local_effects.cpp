#include "local_effects.hpp"

#include <array>
#include <cmath>
#include <mutex>
#include <string>

#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::vfx::local {

namespace {

namespace integration = kcd2s::integration;
namespace sdk = kcd2s::sdk;

// Orders a script can queue before the engine thread takes them (more are dropped).
constexpr std::size_t kMaxQueued = 256;

std::mutex mutex;
std::vector<integration::VfxOrder> queued;

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

std::string text(const sdk::Value& message, const char* name) {
  const sdk::Value* value = integration::script_field(message, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string();
}

// A point { x, y, z } of the message, or zero.
std::array<float, 3> point(const sdk::Value& message, const char* name) {
  std::array<float, 3> out{};
  const sdk::Value* value = integration::script_field(message, name);
  if (value && value->tag() == sdk::Tag::Map) {
    out[0] = float(number(*value, "x", 0));
    out[1] = float(number(*value, "y", 0));
    out[2] = float(number(*value, "z", 0));
  }
  return out;
}

// A rotation { x, y, z, w } of the message, or the identity.
std::array<float, 4> rotation(const sdk::Value& message) {
  std::array<float, 4> out{0.0f, 0.0f, 0.0f, 1.0f};
  const sdk::Value* value = integration::script_field(message, "rotation");
  if (value && value->tag() == sdk::Tag::Map) {
    out[0] = float(number(*value, "x", 0));
    out[1] = float(number(*value, "y", 0));
    out[2] = float(number(*value, "z", 0));
    out[3] = float(number(*value, "w", 1));
  }
  return out;
}

integration::VfxTuning tuning(const sdk::Value& message) {
  integration::VfxTuning tune;
  tune.scale = float(number(message, "scale", 1));
  tune.count_scale = float(number(message, "countScale", 1));
  tune.speed_scale = float(number(message, "speedScale", 1));
  tune.time_scale = float(number(message, "timeScale", 1));
  tune.strength = float(number(message, "strength", -1));
  tune.pulse_period = float(number(message, "pulsePeriod", 0));
  return tune;
}

bool finite(const std::array<float, 3>& values) {
  return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "vfxLocal") {
    return false;
  }
  const std::string kind = text(message, "kind");
  integration::VfxOrder order;
  if (kind == "spawn") {
    order.kind = integration::VfxKind::Spawn;
    order.id = std::uint32_t(number(message, "id", 0));
    order.position = point(message, "position");
    // Vfx.attach: on a player's or NPC's body, the position then its offset.
    order.body = std::uint32_t(number(message, "entity", 0));
    order.joint = text(message, "joint");
  } else if (kind == "destroy") {
    order.kind = integration::VfxKind::Destroy;
    order.id = std::uint32_t(number(message, "id", 0));
  } else if (kind == "burst") {
    order.kind = integration::VfxKind::Burst;
    order.position = point(message, "position");
  } else if (kind == "burstFrom") {
    order.kind = integration::VfxKind::BurstOn;
    order.id = std::uint32_t(number(message, "entity", 0));
    order.position = point(message, "offset");
    order.joint = text(message, "joint");
  } else {
    return true;
  }
  // Only the ids the client's Vfx hands out name a local effect; a burst on a body names the body.
  const bool own_id =
      order.kind == integration::VfxKind::Spawn || order.kind == integration::VfxKind::Destroy;
  if ((own_id && order.id < kLocalEffectIds) || !finite(order.position)) {
    return true;
  }
  order.effect = text(message, "effect");
  order.rotation = rotation(message);
  order.duration_ms = std::uint32_t(number(message, "durationMs", 0));
  order.tuning = tuning(message);
  const sdk::Value* prime = integration::script_field(message, "prime");
  order.prime = prime && prime->tag() == sdk::Tag::Bool && prime->as_bool();
  std::lock_guard<std::mutex> lock(mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(std::move(order));
  }
  return true;
}

std::vector<integration::VfxOrder> take_orders() {
  std::lock_guard<std::mutex> lock(mutex);
  std::vector<integration::VfxOrder> orders;
  orders.swap(queued);
  return orders;
}

}  // namespace kcds::runtime::vfx::local
