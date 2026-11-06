#include "view_shake.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::camera::shake {

namespace {

namespace integration = kcd2s::integration;
namespace sdk = kcd2s::sdk;

// gFramework GetIViewSystem; IViewSystem GetActiveView; IView SetViewShake (byte offsets).
constexpr std::size_t kFrameworkViews = 232;
constexpr std::size_t kActiveView = 56;
constexpr std::size_t kSetViewShake = 64;
// The shake id KCDS starts its shakes with: a new one replaces the one before (the flow node's
// is 24).
constexpr int kShakeId = 0x4B43;
constexpr double kPi = 3.14159265358979323846;
// The most shakes waiting for one frame.
constexpr std::size_t kMaxQueued = 8;

struct Vec3 {
  float x = 0;
  float y = 0;
  float z = 0;
};

struct Shake {
  Vec3 angle;  // radians
  Vec3 shift;  // metres
  float duration = 0;
  float frequency = 0;
  float randomness = 0;
};

using Get = void*(__fastcall*)(void*);
using SetViewShake = void(__fastcall*)(void* view, const Vec3* angle, const Vec3* shift,
                                       float duration, float frequency, float randomness,
                                       int shake_id, bool flip, bool update_only, bool ground_only);

std::mutex queue_mutex;
std::vector<Shake> queued;

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return *reinterpret_cast<Fn*>(*static_cast<std::uint8_t**>(object) + offset);
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

// A { x, y, z } of the message, each clamped to `most` either way; zeros without one.
Vec3 triple(const sdk::Value& message, const char* name, double most, double scale) {
  Vec3 out;
  const sdk::Value* value = integration::script_field(message, name);
  if (!value || value->tag() != sdk::Tag::Map) {
    return out;
  }
  const auto fitted = [&](const char* axis) {
    const double read = number(*value, axis, 0);
    return float(std::clamp(std::isfinite(read) ? read : 0.0, -most, most) * scale);
  };
  out.x = fitted("x");
  out.y = fitted("y");
  out.z = fitted("z");
  return out;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "cameraShakeLocal") {
    return false;
  }
  // The script's checks ran in Client/resources/camera.cjs; the values are held in range again.
  Shake shake;
  shake.angle = triple(message, "angle", 90, kPi / 180);
  shake.shift = triple(message, "shift", 2, 1);
  shake.duration = float(std::clamp(number(message, "duration", 0.5), 0.01, 10.0));
  shake.frequency = float(std::clamp(number(message, "frequency", 0), 0.0, 100.0));
  shake.randomness = float(std::clamp(number(message, "randomness", 0), 0.0, 1.0));
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(shake);
  }
  return true;
}

void tick(void* framework, bool in_world) {
  std::vector<Shake> shakes;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    shakes.swap(queued);
  }
  if (shakes.empty() || !framework || !in_world) {
    return;
  }
  void* views = slot<Get>(framework, kFrameworkViews)(framework);
  void* view = views ? slot<Get>(views, kActiveView)(views) : nullptr;
  if (!view) {
    return;
  }
  for (const Shake& shake : shakes) {
    // As the flow node: its frequency goes as one over the shakes per second, 0 for one push.
    const float period = shake.frequency > 0 ? 1.0f / shake.frequency : 0.0f;
    slot<SetViewShake>(view, kSetViewShake)(view, &shake.angle, &shake.shift, shake.duration,
                                            period, shake.randomness, kShakeId, true, false, false);
  }
}

}  // namespace kcds::runtime::camera::shake
