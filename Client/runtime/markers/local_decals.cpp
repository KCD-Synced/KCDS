#include "local_decals.hpp"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/integration/script_channel.hpp"
#include "kcd2s/integration/world_markers.hpp"
#include "world_markers.hpp"

namespace kcds::runtime::markers::local_decals {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// Orders kept between two frames at most (a pasted group of decals fits).
constexpr std::size_t kMaxQueued = 1024;
// The decal's size the game takes (metres), as a server marker's.
constexpr float kLeastSize = 0.05f;
constexpr float kMostSize = 64.0f;

struct Order {
  bool show = false;
  integration::MarkerShow marker;
};

std::mutex queue_mutex;
std::vector<Order> queued;

double number(const sdk::Value& value, const char* name, double fallback) {
  const sdk::Value* field = integration::script_field(value, name);
  if (!field) {
    return fallback;
  }
  if (field->tag() == sdk::Tag::Double) {
    return field->as_double();
  }
  if (field->tag() == sdk::Tag::Int) {
    return double(field->as_int());
  }
  return fallback;
}

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "localDecal") {
    return false;
  }
  const double id = number(message, "id", 0);
  if (!(id >= 1 && id < double(kLocalDecalBase))) {
    return true;
  }
  Order order;
  order.marker.marker = kLocalDecalBase + std::uint32_t(id);
  if (text(message, "kind") == "show") {
    const std::string material = text(message, "material");
    const double size = number(message, "size", 2);
    if (material.empty() || material.size() > 256 || !(size >= kLeastSize && size <= kMostSize)) {
      return true;
    }
    order.show = true;
    order.marker.material = material;
    order.marker.shape = "decal";
    order.marker.position[0] = float(number(message, "x", 0));
    order.marker.position[1] = float(number(message, "y", 0));
    order.marker.position[2] = float(number(message, "z", 0));
    order.marker.size = float(size);
    // Turned about the up axis by `turn` degrees: the decal lies flat on the ground.
    const double half = number(message, "turn", 0) * 3.14159265358979 / 360.0;
    order.marker.rotation[2] = float(std::sin(half));
    order.marker.rotation[3] = float(std::cos(half));
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(std::move(order));
  }
  return true;
}

void frame(bool in_world) {
  if (!in_world) {
    return;
  }
  std::vector<Order> orders;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    orders.swap(queued);
  }
  for (const Order& order : orders) {
    if (order.show) {
      markers::show(order.marker);
    } else {
      markers::hide(order.marker.marker);
    }
  }
}

void reset() {
  std::lock_guard<std::mutex> lock(queue_mutex);
  queued.clear();
}

}  // namespace kcds::runtime::markers::local_decals
