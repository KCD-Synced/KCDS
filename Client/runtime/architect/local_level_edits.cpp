#include "local_level_edits.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::level_edits {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// The editor's edits take ids from here on, a range a server never gives.
constexpr std::uint32_t kFirstId = 0xF0000000u;

std::mutex queue_mutex;
std::vector<integration::LevelEditChange> queued;

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

bool flag(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Bool && field->as_bool();
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "localLevelEdit") {
    return false;
  }
  if (!local_mode::enabled()) {
    return true;
  }
  const double id = number(message, "id", 0);
  if (!(id >= 1 && id < 0x0FFFFFFF)) {
    return true;
  }
  integration::LevelEditChange change;
  change.edit.id = kFirstId + std::uint32_t(id);
  const std::string kind = text(message, "kind");
  if (kind == "remove") {
    change.removed = true;
  } else if (kind == "set") {
    const std::string object = text(message, "object");
    if (object == "entity") {
      change.edit.kind = integration::LevelEditKind::Entity;
      if (!integration::parse_level_edit_guid(text(message, "guid"), change.edit.guid)) {
        return true;
      }
    } else if (object == "brush") {
      change.edit.kind = integration::LevelEditKind::Brush;
    } else {
      return true;
    }
    change.edit.name = text(message, "name");
    const sdk::Value* at = integration::script_field(message, "at");
    if (at && at->tag() == sdk::Tag::Map) {
      change.edit.at[0] = float(number(*at, "x", 0));
      change.edit.at[1] = float(number(*at, "y", 0));
      change.edit.at[2] = float(number(*at, "z", 0));
    }
    change.edit.hidden = flag(message, "hidden");
    // Moved where it stands (a level entity moves itself: a door keeps its swing, a cart its
    // wheels): its new position, rotation (x, y, z, w) and scale, each optional.
    const sdk::Value* position = integration::script_field(message, "position");
    if (position && position->tag() == sdk::Tag::Map) {
      change.edit.has_position = true;
      change.edit.position[0] = float(number(*position, "x", 0));
      change.edit.position[1] = float(number(*position, "y", 0));
      change.edit.position[2] = float(number(*position, "z", 0));
    }
    const sdk::Value* rotation = integration::script_field(message, "rotation");
    if (rotation && rotation->tag() == sdk::Tag::Map) {
      change.edit.has_rotation = true;
      change.edit.rotation[0] = float(number(*rotation, "x", 0));
      change.edit.rotation[1] = float(number(*rotation, "y", 0));
      change.edit.rotation[2] = float(number(*rotation, "z", 0));
      change.edit.rotation[3] = float(number(*rotation, "w", 1));
    }
    const double scale = number(message, "scale", 0);
    if (scale >= 0.1 && scale <= 10.0) {
      change.edit.has_scale = true;
      change.edit.scale[0] = float(scale);
      change.edit.scale[1] = float(scale);
      change.edit.scale[2] = float(scale);
    }
  } else {
    return true;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < integration::kMaxLevelEditChanges) {
    queued.push_back(std::move(change));
  }
  return true;
}

std::vector<integration::LevelEditChange> take() {
  std::vector<integration::LevelEditChange> changes;
  std::lock_guard<std::mutex> lock(queue_mutex);
  changes.swap(queued);
  return changes;
}

}  // namespace kcds::runtime::architect::level_edits
