#include "local_previews.hpp"

#include <cmath>
#include <mutex>
#include <string>
#include <utility>

#include "kcd2s/integration/script_channel.hpp"
#include "placer.hpp"

namespace kcds::runtime::props::local_previews {

namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// The most orders kept between two frames, and reports between two pumps.
constexpr std::size_t kMaxQueued = integration::kMaxPreviewOrders;

std::mutex queue_mutex;
std::vector<integration::GhostOrder> ghost_orders;
std::vector<integration::PlacerOrder> placer_orders;
std::vector<float> turns;
std::vector<integration::PlacerReport> reports;
struct Bounds {
  std::uint32_t ghost = 0;
  bool present = false;
  float min[3]{};
  float max[3]{};
};
std::vector<Bounds> bounds_reports;

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

bool flag(const sdk::Value& value, const char* name, bool fallback) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Bool ? field->as_bool() : fallback;
}

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

std::array<float, 3> point(const sdk::Value* value) {
  std::array<float, 3> out{};
  if (value && value->tag() == sdk::Tag::Map) {
    out = {float(number(*value, "x", 0)), float(number(*value, "y", 0)),
           float(number(*value, "z", 0))};
  }
  return out;
}

std::array<float, 4> turn(const sdk::Value* value) {
  std::array<float, 4> out{0.0f, 0.0f, 0.0f, 1.0f};
  if (value && value->tag() == sdk::Tag::Map) {
    out = {float(number(*value, "x", 0)), float(number(*value, "y", 0)),
           float(number(*value, "z", 0)), float(number(*value, "w", 1))};
  }
  return out;
}

// { r, g, b, a }, or null for none.
integration::PreviewTint tint_of(const sdk::Value* value) {
  integration::PreviewTint tint;
  if (value && value->tag() == sdk::Tag::Map) {
    tint.present = true;
    tint.rgba = {float(number(*value, "r", 1)), float(number(*value, "g", 1)),
                 float(number(*value, "b", 1)), float(number(*value, "a", 1))};
  }
  return tint;
}

const sdk::Value::Array* array(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Array ? &field->as_array() : nullptr;
}

// The pieces of `models`, with the offsets and rotations that line up with them.
std::vector<integration::PreviewPiece> pieces_of(const sdk::Value& message) {
  std::vector<integration::PreviewPiece> pieces;
  const auto* models = array(message, "models");
  const auto* offsets = array(message, "offsets");
  const auto* rotations = array(message, "rotations");
  if (!models) {
    return pieces;
  }
  for (std::size_t k = 0; k < models->size() && k < integration::kMaxPreviewPieces; ++k) {
    const sdk::Value& model = (*models)[k];
    if (model.tag() != sdk::Tag::Str) {
      continue;
    }
    integration::PreviewPiece piece;
    piece.model = model.as_str();
    if (offsets && k < offsets->size()) {
      piece.offset = point(&(*offsets)[k]);
    }
    if (rotations && k < rotations->size()) {
      piece.rotation = turn(&(*rotations)[k]);
    }
    pieces.push_back(std::move(piece));
  }
  return pieces;
}

// A ghost's scale per axis, uniform without one; an axis out of range is refused
// by the ghost's check.
std::array<float, 3> axes_of(const sdk::Value& message) {
  const sdk::Value* axes = integration::script_field(message, "scaleAxes");
  return axes ? point(axes) : std::array<float, 3>{1.0f, 1.0f, 1.0f};
}

bool ghost_order(const sdk::Value& message, integration::GhostOrder& order) {
  const std::string kind = text(message, "kind");
  order.ghost = std::uint32_t(number(message, "ghost", 0)) | kLocalBit;
  if (kind == "create") {
    order.kind = integration::GhostKind::Create;
    order.pieces = pieces_of(message);
    order.position = point(integration::script_field(message, "position"));
    order.rotation = turn(integration::script_field(message, "rotation"));
    order.scale = float(number(message, "scale", 1));
    order.axes = axes_of(message);
    order.tint = tint_of(integration::script_field(message, "tint"));
    order.hidden = flag(message, "hidden", false);
    order.see_through = flag(message, "seeThrough", false);
    return !order.pieces.empty();
  }
  if (kind == "pose") {
    order.kind = integration::GhostKind::Pose;
    order.position = point(integration::script_field(message, "position"));
    order.rotation = turn(integration::script_field(message, "rotation"));
    order.scale = float(number(message, "scale", 1));
    order.axes = axes_of(message);
    return true;
  }
  if (kind == "tint") {
    order.kind = integration::GhostKind::Tint;
    order.tint = tint_of(integration::script_field(message, "tint"));
    return true;
  }
  if (kind == "hidden") {
    order.kind = integration::GhostKind::Hidden;
    order.hidden = flag(message, "hidden", false);
    return true;
  }
  if (kind == "destroy") {
    order.kind = integration::GhostKind::Destroy;
    return true;
  }
  return false;
}

bool placer_order(const sdk::Value& message, integration::PlacerOrder& order) {
  const std::string kind = text(message, "kind");
  order.session = std::uint32_t(number(message, "session", 0)) | kLocalBit;
  if (kind == "begin") {
    order.kind = integration::PlacerKind::Begin;
    order.pieces = pieces_of(message);
    order.scale = float(number(message, "scale", 1));
    const auto valid = tint_of(integration::script_field(message, "validTint"));
    const auto invalid = tint_of(integration::script_field(message, "invalidTint"));
    if (valid.present) {
      order.valid_tint = valid.rgba;
    }
    if (invalid.present) {
      order.invalid_tint = invalid.rgba;
    }
    order.range = float(number(message, "range", 40));
    order.fallback_distance = float(number(message, "fallbackDistance", 3));
    order.vertical_offset = float(number(message, "verticalOffset", 0));
    order.yaw = float(number(message, "yaw", 0));
    order.flags = integration::kPlacerReportMoves;
    if (flag(message, "alignToSurface", false)) {
      order.flags |= integration::kPlacerAlignToSurface;
    }
    if (flag(message, "faceAwayFromPlayer", true)) {
      order.flags |= integration::kPlacerFaceAwayFromPlayer;
    }
    if (flag(message, "keepOpen", true)) {
      order.flags |= integration::kPlacerKeepOpen;
    }
    if (flag(message, "requireSurface", true)) {
      order.flags |= integration::kPlacerRequireSurface;
    }
    order.max_slope = float(number(message, "maxSlope", -1));
    order.min_distance = float(number(message, "minDistance", -1));
    order.max_distance = float(number(message, "maxDistance", -1));
    return !order.pieces.empty();
  }
  if (kind == "veto") {
    order.kind = integration::PlacerKind::Veto;
    order.vetoed = flag(message, "vetoed", false);
    return true;
  }
  if (kind == "end") {
    order.kind = integration::PlacerKind::End;
    return true;
  }
  return false;
}

const char* event_name(integration::PlacerEvent event) {
  switch (event) {
    case integration::PlacerEvent::Move:
      return "move";
    case integration::PlacerEvent::Confirm:
      return "confirm";
    case integration::PlacerEvent::Rejected:
      return "rejected";
    case integration::PlacerEvent::Cancel:
      return "cancel";
    case integration::PlacerEvent::End:
      break;
  }
  return "end";
}

sdk::Value vector_value(const float* v, int count) {
  const char* names[4] = {"x", "y", "z", "w"};
  sdk::Value::Map map;
  for (int k = 0; k < count; ++k) {
    map.emplace_back(names[k], sdk::Value(double(v[k])));
  }
  return sdk::Value(std::move(map));
}

sdk::Value report_value(const integration::PlacerReport& report) {
  sdk::Value::Map message{
      {"op", sdk::Value(std::string("localPlacerReport"))},
      {"session", sdk::Value(std::int64_t(report.session & ~kLocalBit))},
      {"event", sdk::Value(std::string(event_name(report.event)))},
      {"valid", sdk::Value(report.valid)},
  };
  const bool posed = report.event == integration::PlacerEvent::Move ||
                     report.event == integration::PlacerEvent::Confirm ||
                     report.event == integration::PlacerEvent::Rejected;
  if (posed) {
    message.emplace_back("pose", sdk::Value(sdk::Value::Map{
                                     {"position", vector_value(report.position.data(), 3)},
                                     {"rotation", vector_value(report.rotation.data(), 4)},
                                     {"normal", vector_value(report.normal.data(), 3)},
                                     {"distance", sdk::Value(double(report.distance))},
                                     {"onSurface", sdk::Value(report.on_surface)},
                                 }));
  }
  return sdk::Value(std::move(message));
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string op = integration::script_operation(message);
  if (op == "localGhost") {
    integration::GhostOrder order;
    if (ghost_order(message, order)) {
      std::lock_guard<std::mutex> lock(queue_mutex);
      integration::queue_ghost_order(ghost_orders, std::move(order), kMaxQueued);
    }
    return true;
  }
  if (op == "localPlacer") {
    if (text(message, "kind") == "rotate") {
      std::lock_guard<std::mutex> lock(queue_mutex);
      turns.push_back(float(number(message, "degrees", 0)));
      return true;
    }
    integration::PlacerOrder order;
    if (placer_order(message, order)) {
      std::lock_guard<std::mutex> lock(queue_mutex);
      if (placer_orders.size() < kMaxQueued) {
        placer_orders.push_back(std::move(order));
      }
    }
    return true;
  }
  return false;
}

void take(std::vector<integration::GhostOrder>& ghosts,
          std::vector<integration::PlacerOrder>& placements) {
  std::vector<float> taken_turns;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    for (auto& order : ghost_orders) {
      ghosts.push_back(std::move(order));
    }
    for (auto& order : placer_orders) {
      placements.push_back(std::move(order));
    }
    ghost_orders.clear();
    placer_orders.clear();
    taken_turns.swap(turns);
  }
  for (const float degrees : taken_turns) {
    if (std::isfinite(degrees)) {
      placer::rotate(degrees);
    }
  }
}

void report(const integration::PlacerReport& given) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (reports.size() < kMaxQueued) {
    reports.push_back(given);
  }
}

void report_bounds(std::uint32_t ghost, bool present, const float min[3], const float max[3]) {
  Bounds bounds;
  bounds.ghost = ghost & ~kLocalBit;
  bounds.present = present;
  for (int k = 0; k < 3 && present; ++k) {
    bounds.min[k] = min[k];
    bounds.max[k] = max[k];
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (bounds_reports.size() < kMaxQueued) {
    bounds_reports.push_back(bounds);
  }
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<integration::PlacerReport> taken;
  std::vector<Bounds> taken_bounds;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(reports);
    taken_bounds.swap(bounds_reports);
  }
  for (const auto& given : taken) {
    send(report_value(given));
  }
  for (const auto& bounds : taken_bounds) {
    sdk::Value::Map message{
        {"op", sdk::Value(std::string("localGhostBounds"))},
        {"ghost", sdk::Value(std::int64_t(bounds.ghost))},
    };
    if (bounds.present) {
      message.emplace_back("min", vector_value(bounds.min, 3));
      message.emplace_back("max", vector_value(bounds.max, 3));
    }
    send(sdk::Value(std::move(message)));
  }
}

}  // namespace kcds::runtime::props::local_previews
