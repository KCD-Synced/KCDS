#include "studio_event.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "../core/log.hpp"
#include "../crafting/sounds.hpp"
#include "../props/prop_entity.hpp"
#include "../vfx/placed_effects.hpp"
#include "kcd2s/integration/placed_objects.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_view.hpp"

namespace kcds::runtime::studio::events {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr std::uint32_t kBurstMs = 2000;
constexpr std::size_t kLongestName = 128;

struct Event {
  bool sound = false;
  std::string name;
  std::string joint;
  float scale = 1.0f;
};

std::mutex events_mutex;
std::vector<Event> pending;

std::string text_field(const sdk::Value& message, const char* name) {
  const sdk::Value* field = integration::script_field(message, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

void play_effect(void* entity, const Event& event) {
  integration::VfxOrder order;
  order.kind = integration::VfxKind::Burst;
  order.effect = event.name;
  order.duration_ms = kBurstMs;
  order.tuning.scale = event.scale;
  float place[3];
  float turn[4];
  const bool at_joint =
      !event.joint.empty() && props::entity::joint_pose(entity, event.joint.c_str(), place, turn);
  if (!at_joint) {
    props::entity::world_pose(entity, place, turn);
  }
  order.position = {place[0], place[1], place[2]};
  game_client::log("STUDIO event: effect %s at %s (%.2f %.2f %.2f)", event.name.c_str(),
                   at_joint ? event.joint.c_str() : "the body", place[0], place[1], place[2]);
  vfx::placed::apply(order);
}

void play_sound(void* entity, const Event& event) {
  const std::uint32_t trigger = crafting::sounds::trigger_id(event.name.c_str());
  if (trigger == 0) {
    game_client::log("STUDIO event: no sound trigger %s", event.name.c_str());
    return;
  }
  crafting::sounds::play(entity, trigger, true);
  game_client::log("STUDIO event: sound %s (trigger %u) on the character", event.name.c_str(),
                   trigger);
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "studioEvent") {
    return false;
  }
  Event event;
  event.sound = text_field(message, "kind") == "sound";
  event.name = text_field(message, "name");
  event.joint = text_field(message, "joint");
  const sdk::Value* scale = integration::script_field(message, "scale");
  double size = 1.0;
  if (scale && view::number_of(*scale, size)) {
    event.scale = std::clamp(float(size), 0.01f, 32.0f);
  }
  if (event.name.empty() || event.name.size() > kLongestName || event.joint.size() > 64) {
    return true;
  }
  std::lock_guard<std::mutex> lock(events_mutex);
  if (pending.size() < 32) {
    pending.push_back(event);
  }
  return true;
}

void tick(void* local_entity) {
  std::vector<Event> now;
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    now.swap(pending);
  }
  if (!local_entity) {
    return;
  }
  for (const Event& event : now) {
    if (event.sound) {
      play_sound(local_entity, event);
    } else {
      play_effect(local_entity, event);
    }
  }
}

}  // namespace kcds::runtime::studio::events
