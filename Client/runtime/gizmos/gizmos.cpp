#include "gizmos.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../props/pose_math.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::gizmos {
namespace {

using kcd2s::integration::GizmoKind;
using kcd2s::integration::GizmoOrder;
using kcd2s::integration::GizmoShape;
namespace sdk = kcd2s::sdk;

constexpr std::size_t kGetAuxGeom = 1592 / 8;
constexpr std::size_t kSetRenderFlags = 8 / 8;
constexpr std::size_t kDrawLine = 64 / 8;
constexpr std::size_t kDrawTriangle = 136 / 8;
// The flags CScriptBind_System::DrawLine sets before it draws.
constexpr std::uint32_t kRenderFlags = 0x01000000;
// The client drawings' flags, as the aux geometry's state setup reads them: faces
// seen from both sides, no depth write, no depth test, blended.
constexpr std::uint32_t kNoCulling = 0x00800000;
constexpr std::uint32_t kNoDepthWrite = 0x00400000;
constexpr std::uint32_t kNoDepthTest = 0x00200000;
constexpr std::uint32_t kBlended = 0x20000000;
constexpr float kThickness = 1.0f;
constexpr float kThinLineShare = 0.6f;
// What one client drawing may hold (the resource runtime keeps every drawing within 16384
// geometry units together, a line one and a triangle three).
constexpr std::size_t kMaxPoints = 16384;
constexpr std::size_t kMaxIndices = 32768;
constexpr std::size_t kMaxQueued = 1024;
constexpr int kCircleSegments = 24;
constexpr float kPi = 3.14159265f;

struct ColorB {
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
  std::uint8_t a;
};

using GetAuxGeom = void*(__fastcall*)(void* renderer);
using SetRenderFlags = std::uint32_t*(__fastcall*)(void* aux, std::uint32_t* previous,
                                                   const std::uint32_t* flags);
using DrawLine = void(__fastcall*)(void* aux, const float* from, const ColorB* from_color,
                                   const float* to, const ColorB* to_color, float thickness);
using DrawLabel = void(__fastcall*)(const float* position, float size, const float* color,
                                    bool fixed_size, bool centred, const char* text);
using DrawTriangle = void(__fastcall*)(void* aux, const float* a, const ColorB* a_color,
                                       const float* b, const ColorB* b_color, const float* c,
                                       const ColorB* c_color);

struct Live {
  GizmoOrder order;
  double until = 0;  // 0: until cleared
};

// A client resource's drawing: its points as authored, drawn at position + rotation * p.
struct Drawing {
  std::vector<float> points;  // x, y, z in a row
  std::vector<std::uint32_t> lines;
  std::vector<std::uint32_t> triangles;
  std::vector<ColorB> line_colors;  // one per line, or none
  std::vector<bool> thin;           // one per line, or none
  ColorB color{};
  ColorB fill{};
  bool filled = false;
  float width = 1.5f;
  bool depth_test = true;
  double until = 0;  // 0: until removed
  float position[3] = {0.0f, 0.0f, 0.0f};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

enum class RequestKind { Set, Transform, Remove, RemoveOwner };

struct Request {
  RequestKind kind = RequestKind::Set;
  std::string key;  // owner '\n' id; the owner alone for RemoveOwner
  Drawing drawing;
  std::uint32_t duration_ms = 0;
};

Natives natives;
Logger log_line = nullptr;
bool ready = false;
// Engine thread only: gizmo id -> what is drawn.
std::unordered_map<std::uint32_t, Live> live;
// Engine thread only: owner '\n' id -> client drawing, and the level they belong to.
std::map<std::string, Drawing> drawings;
std::string drawings_level;
std::mutex queue_mutex;
std::vector<Request> queued;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

std::uint8_t byte_of(float channel) {
  const float clamped = std::fmin(1.0f, std::fmax(0.0f, channel));
  return std::uint8_t(clamped * 255.0f + 0.5f);
}

ColorB color_of(const float rgba[4]) {
  return ColorB{byte_of(rgba[0]), byte_of(rgba[1]), byte_of(rgba[2]), byte_of(rgba[3])};
}

// The aux geometry of this frame with the binding's flags set, or null.
void* aux_geometry() {
  void* renderer = ready && natives.renderer ? *natives.renderer : nullptr;
  if (!renderer) {
    return nullptr;
  }
  void* aux = slot<GetAuxGeom>(renderer, kGetAuxGeom)(renderer);
  if (aux) {
    std::uint32_t previous = 0;
    slot<SetRenderFlags>(aux, kSetRenderFlags)(aux, &previous, &kRenderFlags);
  }
  return aux;
}

void draw_line(void* aux, const float from[3], const float to[3], const ColorB& color) {
  slot<DrawLine>(aux, kDrawLine)(aux, from, &color, to, &color, kThickness);
}

// A circle about `center` in the plane of the unit vectors `u` and `v`.
void draw_ring(void* aux, const float center[3], float radius, const float u[3], const float v[3],
               const ColorB& color) {
  float previous[3] = {center[0] + radius * u[0], center[1] + radius * u[1],
                       center[2] + radius * u[2]};
  for (int i = 1; i <= kCircleSegments; ++i) {
    const float angle = 2.0f * kPi * float(i) / float(kCircleSegments);
    const float c = std::cos(angle) * radius;
    const float s = std::sin(angle) * radius;
    const float point[3] = {center[0] + c * u[0] + s * v[0], center[1] + c * u[1] + s * v[1],
                            center[2] + c * u[2] + s * v[2]};
    draw_line(aux, previous, point, color);
    previous[0] = point[0];
    previous[1] = point[1];
    previous[2] = point[2];
  }
}

void draw_sphere(void* aux, const GizmoOrder& order, const ColorB& color) {
  const float x[3] = {1.0f, 0.0f, 0.0f};
  const float y[3] = {0.0f, 1.0f, 0.0f};
  const float z[3] = {0.0f, 0.0f, 1.0f};
  draw_ring(aux, order.position.data(), order.size, x, y, color);
  draw_ring(aux, order.position.data(), order.size, x, z, color);
  draw_ring(aux, order.position.data(), order.size, y, z, color);
}

void draw_box(void* aux, const GizmoOrder& order, const ColorB& color) {
  float corners[8][3];
  for (int i = 0; i < 8; ++i) {
    const float local[3] = {(i & 1) ? order.extent[0] : -order.extent[0],
                            (i & 2) ? order.extent[1] : -order.extent[1],
                            (i & 4) ? order.extent[2] : -order.extent[2]};
    float turned[3];
    pose_math::rotate(order.rotation.data(), local, turned);
    for (int axis = 0; axis < 3; ++axis) {
      corners[i][axis] = order.position[axis] + turned[axis];
    }
  }
  // Each edge joins two corners that differ in one axis bit.
  for (int i = 0; i < 8; ++i) {
    for (int bit = 1; bit < 8; bit <<= 1) {
      if ((i & bit) == 0) {
        draw_line(aux, corners[i], corners[i | bit], color);
      }
    }
  }
}

ColorB color_of_argb(std::uint32_t argb) {
  return ColorB{std::uint8_t(argb >> 16), std::uint8_t(argb >> 8), std::uint8_t(argb),
                std::uint8_t(argb >> 24)};
}

void set_flags(void* aux, std::uint32_t flags) {
  std::uint32_t previous = 0;
  slot<SetRenderFlags>(aux, kSetRenderFlags)(aux, &previous, &flags);
}

std::uint32_t flags_for(const Drawing& drawing, const ColorB& color) {
  std::uint32_t flags = kNoCulling | kNoDepthWrite;
  if (!drawing.depth_test) {
    flags |= kNoDepthTest;
  }
  if (color.a != 255) {
    flags |= kBlended;
  }
  return flags;
}

// A client drawing's points where they are drawn.
std::vector<float> placed_points(const Drawing& drawing) {
  std::vector<float> placed(drawing.points.size());
  for (std::size_t i = 0; i + 2 < drawing.points.size(); i += 3) {
    float turned[3];
    pose_math::rotate(drawing.rotation, &drawing.points[i], turned);
    for (int axis = 0; axis < 3; ++axis) {
      placed[i + axis] = drawing.position[axis] + turned[axis];
    }
  }
  return placed;
}

void draw_client(void* aux, const Drawing& drawing) {
  const std::vector<float> placed = placed_points(drawing);
  if (drawing.filled && !drawing.triangles.empty()) {
    set_flags(aux, flags_for(drawing, drawing.fill));
    const auto triangle = slot<DrawTriangle>(aux, kDrawTriangle);
    for (std::size_t i = 0; i + 2 < drawing.triangles.size(); i += 3) {
      triangle(aux, &placed[3 * drawing.triangles[i]], &drawing.fill,
               &placed[3 * drawing.triangles[i + 1]], &drawing.fill,
               &placed[3 * drawing.triangles[i + 2]], &drawing.fill);
    }
  }
  const auto draw_segment = slot<DrawLine>(aux, kDrawLine);
  std::uint32_t flags = 0;
  for (std::size_t line = 0; 2 * line + 1 < drawing.lines.size(); ++line) {
    const ColorB& color = drawing.line_colors.empty() ? drawing.color : drawing.line_colors[line];
    const std::uint32_t wanted = flags_for(drawing, color);
    if (wanted != flags) {
      set_flags(aux, wanted);
      flags = wanted;
    }
    const bool thin = !drawing.thin.empty() && drawing.thin[line];
    const float width = thin ? drawing.width * kThinLineShare : drawing.width;
    draw_segment(aux, &placed[3 * drawing.lines[2 * line]], &color,
                 &placed[3 * drawing.lines[2 * line + 1]], &color, width);
  }
  // The server's gizmos draw after these with the binding's own flags.
  set_flags(aux, kRenderFlags);
}

double number(const sdk::Value* value) {
  if (!value) {
    return 0;
  }
  if (value->tag() == sdk::Tag::Double) {
    return value->as_double();
  }
  if (value->tag() == sdk::Tag::Int) {
    return double(value->as_int());
  }
  return 0;
}

std::string text(const sdk::Value& message, const char* name) {
  const sdk::Value* value = kcd2s::integration::script_field(message, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string{};
}

bool flag(const sdk::Value& message, const char* name, bool fallback) {
  const sdk::Value* value = kcd2s::integration::script_field(message, name);
  return value && value->tag() == sdk::Tag::Bool ? value->as_bool() : fallback;
}

// The numbers of an array field; false when it is not an array of at most `max` numbers.
bool numbers(const sdk::Value& message, const char* name, std::size_t max,
             std::vector<double>& out) {
  out.clear();
  const sdk::Value* value = kcd2s::integration::script_field(message, name);
  if (!value) {
    return true;
  }
  if (value->tag() != sdk::Tag::Array || value->as_array().size() > max) {
    return false;
  }
  for (const sdk::Value& item : value->as_array()) {
    if (item.tag() != sdk::Tag::Int && item.tag() != sdk::Tag::Double) {
      return false;
    }
    out.push_back(number(&item));
  }
  return true;
}

// Indices of `count` points; false when one is out of range.
bool indices(const std::vector<double>& given, std::size_t count, std::vector<std::uint32_t>& out) {
  out.clear();
  for (const double index : given) {
    if (!(index >= 0.0) || index >= double(count) || index != std::floor(index)) {
      return false;
    }
    out.push_back(std::uint32_t(index));
  }
  return true;
}

bool read_vector(const sdk::Value& message, const char* name, float* out, int count) {
  const sdk::Value* value = kcd2s::integration::script_field(message, name);
  if (!value || value->tag() != sdk::Tag::Map) {
    return false;
  }
  static const char* const kAxes[] = {"x", "y", "z", "w"};
  for (int i = 0; i < count; ++i) {
    const sdk::Value* axis = kcd2s::integration::script_field(*value, kAxes[i]);
    if (!axis || (axis->tag() != sdk::Tag::Int && axis->tag() != sdk::Tag::Double)) {
      return false;
    }
    out[i] = float(number(axis));
    if (!std::isfinite(out[i])) {
      return false;
    }
  }
  return true;
}

// A gizmoSet's drawing; false when it is malformed.
bool read_drawing(const sdk::Value& message, Drawing& drawing, std::uint32_t& duration_ms) {
  std::vector<double> values;
  if (!numbers(message, "points", 3 * kMaxPoints, values) || values.size() % 3 != 0) {
    return false;
  }
  for (const double value : values) {
    if (!std::isfinite(value)) {
      return false;
    }
    drawing.points.push_back(float(value));
  }
  const std::size_t count = drawing.points.size() / 3;
  if (!numbers(message, "lines", kMaxIndices, values) || values.size() % 2 != 0 ||
      !indices(values, count, drawing.lines)) {
    return false;
  }
  if (!numbers(message, "triangles", kMaxIndices, values) || values.size() % 3 != 0 ||
      !indices(values, count, drawing.triangles)) {
    return false;
  }
  const std::size_t line_count = drawing.lines.size() / 2;
  if (!numbers(message, "lineColors", kMaxIndices, values) ||
      (!values.empty() && values.size() != line_count)) {
    return false;
  }
  for (const double value : values) {
    drawing.line_colors.push_back(color_of_argb(std::uint32_t(value)));
  }
  std::vector<std::uint32_t> thin_lines;
  if (!numbers(message, "thinLines", kMaxIndices, values) ||
      !indices(values, line_count, thin_lines)) {
    return false;
  }
  if (!thin_lines.empty()) {
    drawing.thin.assign(line_count, false);
    for (const std::uint32_t line : thin_lines) {
      drawing.thin[line] = true;
    }
  }
  const double fill = number(kcd2s::integration::script_field(message, "fill"));
  drawing.color =
      color_of_argb(std::uint32_t(number(kcd2s::integration::script_field(message, "color"))));
  drawing.fill = color_of_argb(std::uint32_t(fill));
  drawing.filled = fill != 0;
  const double width = number(kcd2s::integration::script_field(message, "width"));
  drawing.width = float(std::fmin(8.0, std::fmax(0.5, width)));
  drawing.depth_test = flag(message, "depthTest", true);
  const double duration = number(kcd2s::integration::script_field(message, "durationMs"));
  if (!(duration >= 0.0) || duration > double(kcd2s::integration::kMaxGizmoMilliseconds)) {
    return false;
  }
  duration_ms = std::uint32_t(duration);
  return true;
}

void apply_request(Request& request, double now) {
  switch (request.kind) {
    case RequestKind::Set:
      request.drawing.until =
          request.duration_ms == 0 ? 0 : now + double(request.duration_ms) / 1000.0;
      drawings[request.key] = std::move(request.drawing);
      break;
    case RequestKind::Transform: {
      const auto it = drawings.find(request.key);
      if (it != drawings.end()) {
        std::copy(request.drawing.position, request.drawing.position + 3, it->second.position);
        std::copy(request.drawing.rotation, request.drawing.rotation + 4, it->second.rotation);
      }
      break;
    }
    case RequestKind::Remove:
      drawings.erase(request.key);
      break;
    case RequestKind::RemoveOwner: {
      const std::string prefix = request.key + '\n';
      for (auto it = drawings.lower_bound(prefix);
           it != drawings.end() && it->first.compare(0, prefix.size(), prefix) == 0;) {
        it = drawings.erase(it);
      }
      break;
    }
  }
}

void draw(void* aux, const GizmoOrder& order) {
  const ColorB color = color_of(order.color.data());
  switch (order.shape) {
    case GizmoShape::Line:
      draw_line(aux, order.position.data(), order.extent.data(), color);
      break;
    case GizmoShape::Sphere:
      draw_sphere(aux, order, color);
      break;
    case GizmoShape::Box:
      draw_box(aux, order, color);
      break;
    case GizmoShape::Text:
      reinterpret_cast<DrawLabel>(natives.draw_label)(
          order.position.data(), order.size, order.color.data(), true, true, order.text.c_str());
      break;
  }
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  ready = natives.renderer && natives.layout_verified && natives.draw_label;
  return ready;
}

void apply(const GizmoOrder& order, double now) {
  switch (order.kind) {
    case GizmoKind::Draw: {
      const bool known = live.count(order.gizmo) != 0;
      if (!known && live.size() >= kcd2s::integration::kMaxGizmos) {
        return;
      }
      Live& entry = live[order.gizmo];
      entry.order = order;
      entry.until = order.duration_ms == 0 ? 0 : now + double(order.duration_ms) / 1000.0;
      break;
    }
    case GizmoKind::Clear:
      live.erase(order.gizmo);
      break;
    case GizmoKind::ClearAll:
      live.clear();
      break;
  }
}

void frame(double now) {
  std::vector<Request> requests;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    requests.swap(queued);
  }
  for (Request& request : requests) {
    apply_request(request, now);
  }
  if (live.empty() && drawings.empty()) {
    return;
  }
  void* aux = aux_geometry();
  for (auto it = drawings.begin(); it != drawings.end();) {
    if (it->second.until != 0 && now >= it->second.until) {
      it = drawings.erase(it);
      continue;
    }
    if (aux) {
      draw_client(aux, it->second);
    }
    ++it;
  }
  for (auto it = live.begin(); it != live.end();) {
    if (it->second.until != 0 && now >= it->second.until) {
      it = live.erase(it);
      continue;
    }
    if (aux) {
      draw(aux, it->second.order);
    }
    ++it;
  }
}

void reset() {
  live.clear();
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  if (operation.rfind("gizmo", 0) != 0) {
    return false;
  }
  Request request;
  const std::string owner = text(message, "owner");
  if (owner.empty()) {
    return true;
  }
  request.key = owner + '\n' + text(message, "id");
  if (operation == "gizmoSet") {
    request.kind = RequestKind::Set;
    if (!read_drawing(message, request.drawing, request.duration_ms)) {
      if (log_line) {
        log_line("GIZMO %s: malformed drawing dropped", request.key.c_str());
      }
      return true;
    }
  } else if (operation == "gizmoTransform") {
    request.kind = RequestKind::Transform;
    if (!read_vector(message, "position", request.drawing.position, 3) ||
        !read_vector(message, "rotation", request.drawing.rotation, 4)) {
      return true;
    }
  } else if (operation == "gizmoRemove") {
    request.kind = RequestKind::Remove;
  } else if (operation == "gizmoRemoveOwner") {
    request.kind = RequestKind::RemoveOwner;
    request.key = owner;
  } else {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(std::move(request));
  }
  return true;
}

bool level(const std::string& map) {
  if (map == drawings_level) {
    return false;
  }
  // The first world of a session keeps what the resources drew while it loaded.
  const bool left_level = !drawings_level.empty();
  drawings_level = map;
  if (!left_level) {
    return false;
  }
  drawings.clear();
  std::lock_guard<std::mutex> lock(queue_mutex);
  queued.clear();
  return true;
}

void line(const float from[3], const float to[3], const float rgba[4]) {
  if (void* aux = aux_geometry()) {
    draw_line(aux, from, to, color_of(rgba));
  }
}

void line(const float from[3], const float to[3], const float rgba[4], float thickness) {
  void* aux = aux_geometry();
  if (!aux) {
    return;
  }
  set_flags(aux, kNoCulling | kNoDepthWrite | kNoDepthTest | kBlended);
  const ColorB color = color_of(rgba);
  slot<DrawLine>(aux, kDrawLine)(aux, from, &color, to, &color, thickness);
  set_flags(aux, kRenderFlags);
}

void circle(const float center[3], float radius, const float rgba[4]) {
  if (void* aux = aux_geometry()) {
    const float x[3] = {1.0f, 0.0f, 0.0f};
    const float y[3] = {0.0f, 1.0f, 0.0f};
    draw_ring(aux, center, radius, x, y, color_of(rgba));
  }
}

void label(const float position[3], float size, const float rgba[4], const char* text) {
  if (ready && natives.draw_label && text) {
    reinterpret_cast<DrawLabel>(natives.draw_label)(position, size, rgba, true, true, text);
  }
}

void solid(const std::vector<float>& points, const std::vector<std::uint32_t>& triangles,
           const std::vector<std::uint32_t>& lines, std::uint32_t fill, std::uint32_t edge) {
  void* aux = aux_geometry();
  if (!aux) {
    return;
  }
  Drawing drawing;
  drawing.points = points;
  drawing.triangles = triangles;
  drawing.lines = lines;
  drawing.fill = color_of_argb(fill);
  drawing.filled = true;
  drawing.color = color_of_argb(edge);
  drawing.width = 1.0f;
  draw_client(aux, drawing);
}

}  // namespace kcds::runtime::gizmos
