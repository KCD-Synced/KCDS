#include "player_body.hpp"

#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

#include "../core/log.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::player_body {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// What the editor asked for: the body hidden, and whether it must still be shown once.
std::atomic<bool> hidden{false};
std::atomic<bool> show_pending{false};

// Where Play as player starts the body (the message's `start`: x, y, z and yaw in degrees from +Y,
// rising to the left), until the next frame puts it there.
struct Start {
  float position[3] = {};
  float yaw = 0;
};
std::mutex start_lock;
std::optional<Start> start_pending;

std::optional<double> number_of(const sdk::Value* map, const char* name) {
  const sdk::Value* field = map ? integration::script_field(*map, name) : nullptr;
  if (field && field->tag() == sdk::Tag::Double) {
    return field->as_double();
  }
  if (field && field->tag() == sdk::Tag::Int) {
    return static_cast<double>(field->as_int());
  }
  return std::nullopt;
}

std::optional<Start> start_of(const sdk::Value& message) {
  const sdk::Value* start = integration::script_field(message, "start");
  const auto x = number_of(start, "x");
  const auto y = number_of(start, "y");
  const auto z = number_of(start, "z");
  if (!x || !y || !z || !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z)) {
    return std::nullopt;
  }
  Start found;
  found.position[0] = static_cast<float>(*x);
  found.position[1] = static_cast<float>(*y);
  found.position[2] = static_cast<float>(*z);
  const auto yaw = number_of(start, "yaw");
  found.yaw = yaw && std::isfinite(*yaw) ? static_cast<float>(*yaw) : 0.0f;
  return found;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "architectPlayer") {
    return false;
  }
  if (!local_mode::enabled()) {
    return true;
  }
  const sdk::Value* field = integration::script_field(message, "hidden");
  const bool hide = field && field->tag() == sdk::Tag::Bool && field->as_bool();
  {
    std::lock_guard<std::mutex> guard(start_lock);
    start_pending = hide ? std::nullopt : start_of(message);
  }
  hidden.store(hide);
  show_pending.store(!hide);
  return true;
}

bool editor_open() {
  return hidden.load();
}

void frame(void* local_entity) {
  if (!local_entity) {
    return;
  }
  if (hidden.load()) {
    // Kept hidden: the game may show the body again (a respawn, a cutscene).
    if (!props::entity::is_hidden(local_entity)) {
      props::entity::hide(local_entity, true);
    }
    return;
  }
  if (show_pending.exchange(false) && props::entity::is_hidden(local_entity)) {
    props::entity::hide(local_entity, false);
  }
  std::optional<Start> start;
  {
    std::lock_guard<std::mutex> guard(start_lock);
    start = std::exchange(start_pending, std::nullopt);
  }
  if (start) {
    // Turned about the up axis by the camera's yaw, standing on the ground found under it.
    const float half = start->yaw * 3.14159265f / 360.0f;
    const float rotation[4] = {0.0f, 0.0f, std::sin(half), std::cos(half)};
    props::entity::place_in_world(local_entity, start->position, rotation);
    kcds::game_client::log("ARCHITECT play starts at %.2f %.2f %.2f yaw %.1f", start->position[0],
                           start->position[1], start->position[2], start->yaw);
  }
}

}  // namespace kcds::runtime::architect::player_body
