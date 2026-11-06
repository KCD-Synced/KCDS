#include "whip_lash.hpp"

#include <algorithm>
#include <cmath>

namespace kcds::runtime::physical_props {
namespace {

constexpr float kStep = 1.0f / 300.0f;
constexpr int kMostSteps = 30;
constexpr int kConstraintRounds = 6;
// How fast a point's speed is pulled toward its neighbours' (per second): it takes the jitter out
// of the lash and keeps it a smooth curve (offline: 15 degrees a joint against 22 without).
constexpr float kInnerDamping = 16.0f;
// The least of the handle's bending stiffness a point keeps toward the tip.
constexpr float kTipBend = 0.1f;
// The farthest the hand goes in one frame and still swings the lash (metres; a strike's hand
// moves some 0.2 m in a frame); farther, the lash is carried along (step).
constexpr float kJump = 0.5f;

// Keeps a and b `rest` apart, `share` of the way, the lighter point giving more.
void keep_apart(float* a, float* b, float wa, float wb, float rest, float share) {
  float gap[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const float length = std::sqrt(gap[0] * gap[0] + gap[1] * gap[1] + gap[2] * gap[2]);
  if (length < 1e-6f || wa + wb <= 0.0f) {
    return;
  }
  const float pull = (length - rest) / length / (wa + wb) * share;
  for (int axis = 0; axis < 3; ++axis) {
    a[axis] += wa * pull * gap[axis];
    b[axis] -= wb * pull * gap[axis];
  }
}

float* at(std::vector<float>& points, int index) {
  return &points[std::size_t(index) * 3];
}

// Puts the point out of the capsule (onto its skin) when it is inside.
void push_out(float* point, const LashBody& body) {
  float line[3];
  float from_a[3];
  for (int axis = 0; axis < 3; ++axis) {
    line[axis] = body.b[axis] - body.a[axis];
    from_a[axis] = point[axis] - body.a[axis];
  }
  const float length = line[0] * line[0] + line[1] * line[1] + line[2] * line[2];
  float share = 0.0f;
  if (length > 1e-8f) {
    share = std::clamp((from_a[0] * line[0] + from_a[1] * line[1] + from_a[2] * line[2]) / length,
                       0.0f, 1.0f);
  }
  float away[3];
  for (int axis = 0; axis < 3; ++axis) {
    away[axis] = point[axis] - (body.a[axis] + line[axis] * share);
  }
  const float distance = std::sqrt(away[0] * away[0] + away[1] * away[1] + away[2] * away[2]);
  if (distance >= body.radius || distance < 1e-6f) {
    return;
  }
  const float out = body.radius / distance;
  for (int axis = 0; axis < 3; ++axis) {
    point[axis] = point[axis] - away[axis] + away[axis] * out;
  }
}

// `up` made square to `along` (both unit vectors after); false when they lie along each other.
bool square_up(const float along[3], float up[3]) {
  const float dot = along[0] * up[0] + along[1] * up[1] + along[2] * up[2];
  for (int axis = 0; axis < 3; ++axis) {
    up[axis] -= along[axis] * dot;
  }
  const float length = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
  if (length < 1e-4f) {
    return false;
  }
  for (int axis = 0; axis < 3; ++axis) {
    up[axis] /= length;
  }
  return true;
}

}  // namespace

void WhipLash::posed(const float start[3], const float direction[3], const float up_in[3],
                     const WhipLashSettings& settings, std::vector<float>& points) const {
  constexpr float kRadians = 3.14159265f / 180.0f;
  const int count = int(points_.size() / 3);
  points.assign(points_.size(), 0.0f);
  float up[3] = {up_in[0], up_in[1], up_in[2]};
  if (!square_up(direction, up)) {
    up[0] = 0.0f;
    up[1] = 0.0f;
    up[2] = 1.0f;
    if (!square_up(direction, up)) {
      up[0] = 1.0f;
      up[2] = 0.0f;
      square_up(direction, up);
    }
  }
  // The handle's right side: along × up.
  const float side[3] = {direction[1] * up[2] - direction[2] * up[1],
                         direction[2] * up[0] - direction[0] * up[2],
                         direction[0] * up[1] - direction[1] * up[0]};
  float* point = &points[0];
  for (int axis = 0; axis < 3; ++axis) {
    point[axis] = start[axis];
  }
  for (int segment = 0; segment + 1 < count; ++segment) {
    // Bent evenly along it: this segment's middle is this share of the way to the tip.
    const float share = (float(segment) + 0.5f) / float(count - 1);
    const float curl = settings.curl * kRadians * share;
    const float sway = settings.sway * kRadians * share;
    const float* from = &points[std::size_t(segment) * 3];
    float* to = &points[std::size_t(segment + 1) * 3];
    for (int axis = 0; axis < 3; ++axis) {
      const float across = std::cos(sway) * direction[axis] + std::sin(sway) * side[axis];
      const float way = std::cos(curl) * across + std::sin(curl) * up[axis];
      to[axis] = from[axis] + way * segment_;
    }
  }
}

void WhipLash::shaped(const float start[3], const float direction[3], const float up_in[3],
                      const std::vector<std::array<float, 3>>& shape) {
  if (points_.empty() || shape.empty()) {
    return;
  }
  float up[3] = {up_in[0], up_in[1], up_in[2]};
  if (!square_up(direction, up)) {
    return;
  }
  const float side[3] = {direction[1] * up[2] - direction[2] * up[1],
                         direction[2] * up[0] - direction[0] * up[2],
                         direction[0] * up[1] - direction[1] * up[0]};
  // The shape in the world: the handle's tip, then its points.
  std::vector<std::array<float, 3>> line;
  line.push_back({start[0], start[1], start[2]});
  for (const auto& point : shape) {
    std::array<float, 3> world{};
    for (int axis = 0; axis < 3; ++axis) {
      world[std::size_t(axis)] =
          start[axis] + direction[axis] * point[0] + side[axis] * point[1] + up[axis] * point[2];
    }
    line.push_back(world);
  }
  before_ = points_;
  const int count = int(points_.size() / 3);
  // Along the shape's line, a segment apart; past its end straight on along its last piece.
  for (int point = 0; point < count; ++point) {
    const float wanted = float(point) * segment_;
    float walked = 0.0f;
    std::size_t piece = 0;
    float into = 0.0f;
    while (piece + 1 < line.size()) {
      const auto& a = line[piece];
      const auto& b = line[piece + 1];
      const float length = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) +
                                     (b[2] - a[2]) * (b[2] - a[2]));
      if (walked + length >= wanted || piece + 2 == line.size()) {
        into = length > 1e-6f ? (wanted - walked) / length : 0.0f;
        break;
      }
      walked += length;
      ++piece;
    }
    const auto& a = line[piece];
    const auto& b = line[std::min(piece + 1, line.size() - 1)];
    float* to = at(points_, point);
    for (int axis = 0; axis < 3; ++axis) {
      to[axis] = a[std::size_t(axis)] + (b[std::size_t(axis)] - a[std::size_t(axis)]) * into;
    }
  }
  for (int axis = 0; axis < 3; ++axis) {
    start_[axis] = start[axis];
    direction_[axis] = direction[axis];
    up_[axis] = up_in[axis];
  }
}

void WhipLash::reset(int segments, float length, const float start[3]) {
  const int count = std::max(2, segments) + 1;
  segment_ = length / float(count - 1);
  points_.assign(std::size_t(count) * 3, 0.0f);
  weights_.assign(std::size_t(count), 0.0f);
  for (int index = 0; index < count; ++index) {
    float* point = at(points_, index);
    point[0] = start[0];
    point[1] = start[1];
    point[2] = start[2] - segment_ * float(index);
  }
  before_ = points_;
  speeds_.assign(points_.size(), 0.0f);
  smoothed_.assign(points_.size(), 0.0f);
  for (int axis = 0; axis < 3; ++axis) {
    start_[axis] = start[axis];
  }
}

void WhipLash::step(const float start[3], const float direction[3], const float up[3],
                    float seconds, float ground, const WhipLashSettings& settings) {
  if (points_.empty()) {
    return;
  }
  const int count = int(points_.size() / 3);
  // The weights (1 / mass): the start is held, then lighter toward the tip.
  for (int index = 1; index < count; ++index) {
    const float share = float(index) / float(count - 1);
    const float mass = 1.0f - (1.0f - settings.taper) * share;
    weights_[std::size_t(index)] = 1.0f / mass;
  }
  weights_[0] = 0.0f;
  // The hand jumped (a body placed anew, a replica's clip started again, a teleport): the whole
  // lash goes with it, as it was. Dragged that far its segments stretched into straight rods
  // metres long until they caught up (live in the game, on the other players).
  const float jump[3] = {start[0] - start_[0], start[1] - start_[1], start[2] - start_[2]};
  if (jump[0] * jump[0] + jump[1] * jump[1] + jump[2] * jump[2] > kJump * kJump) {
    for (std::size_t part = 0; part < points_.size(); ++part) {
      points_[part] += jump[part % 3];
      before_[part] += jump[part % 3];
    }
    for (int axis = 0; axis < 3; ++axis) {
      start_[axis] = start[axis];
      direction_[axis] = direction[axis];
      up_[axis] = up[axis];
    }
  }
  const int steps = std::clamp(int(std::ceil(seconds / kStep)), 1, kMostSteps);
  const float step = std::min(seconds, kStep * float(kMostSteps)) / float(steps);
  const float keep = std::max(0.0f, 1.0f - settings.drag * step);
  const float fall = settings.gravity * step * step;
  const float from[3] = {start_[0], start_[1], start_[2]};
  const float from_direction[3] = {direction_[0], direction_[1], direction_[2]};
  const float from_up[3] = {up_[0], up_[1], up_[2]};
  // How far each step pulls the lash toward its posed shape (WhipLashSettings shape).
  const float shape = std::clamp(settings.shape, 0.0f, 1.0f);
  const float pull = shape * shape * shape;
  for (int index = 1; index <= steps; ++index) {
    const float share = float(index) / float(steps);
    // The handle's tip, long side and up side this far through the frame.
    float handle[3];
    float along[3];
    float handle_up[3];
    for (int axis = 0; axis < 3; ++axis) {
      handle[axis] = from[axis] + (start[axis] - from[axis]) * share;
      along[axis] = from_direction[axis] + (direction[axis] - from_direction[axis]) * share;
      handle_up[axis] = from_up[axis] + (up[axis] - from_up[axis]) * share;
    }
    const float along_length =
        std::sqrt(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
    for (float& part : along) {
      part = along_length > 1e-6f ? part / along_length : 0.0f;
    }
    // Their speeds, each pulled toward its neighbours' (inner damping).
    speeds_.assign(points_.size(), 0.0f);
    for (std::size_t part = 0; part < points_.size(); ++part) {
      speeds_[part] = points_[part] - before_[part];
    }
    const float inner = std::min(1.0f, kInnerDamping * step);
    for (int point = 1; point + 1 < count; ++point) {
      for (int axis = 0; axis < 3; ++axis) {
        const float own = speeds_[std::size_t(point) * 3 + std::size_t(axis)];
        const float mean = 0.5f * (speeds_[std::size_t(point - 1) * 3 + std::size_t(axis)] +
                                   speeds_[std::size_t(point + 1) * 3 + std::size_t(axis)]);
        smoothed_[std::size_t(point) * 3 + std::size_t(axis)] = own + (mean - own) * inner;
      }
    }
    // Moving on with their speed, air drag and gravity.
    for (int point = 1; point < count; ++point) {
      float* now = at(points_, point);
      float* was = at(before_, point);
      const bool inside = point + 1 < count;
      for (int axis = 0; axis < 3; ++axis) {
        const std::size_t part = std::size_t(point) * 3 + std::size_t(axis);
        const float speed = (inside ? smoothed_[part] : speeds_[part]) * keep;
        was[axis] = now[axis];
        now[axis] += speed;
      }
      now[2] -= fall;
    }
    float* first = at(points_, 0);
    float* first_was = at(before_, 0);
    for (int axis = 0; axis < 3; ++axis) {
      first_was[axis] = first[axis];
      first[axis] = handle[axis];
    }
    for (int round = 0; round < kConstraintRounds; ++round) {
      // Each segment its length, the lighter point giving more.
      for (int point = 0; point + 1 < count; ++point) {
        keep_apart(at(points_, point), at(points_, point + 1), weights_[std::size_t(point)],
                   weights_[std::size_t(point + 1)], segment_, 1.0f);
      }
      // Bending: the first segment leaves the handle along its long side, and each point keeps
      // two segments from the one two before it (a straight lash), strongly at the handle and
      // not at all at the tip.
      float* second = at(points_, 1);
      const float* start_now = at(points_, 0);
      for (int axis = 0; axis < 3; ++axis) {
        const float wanted = start_now[axis] + along[axis] * segment_;
        second[axis] += (wanted - second[axis]) * settings.stiffness * 0.5f;
      }
      for (int point = 0; point + 2 < count; ++point) {
        const float share = float(point) / float(count - 1);
        // Toward the tip a little stiffness stays (kTipBend of the handle's): with none the
        // tip kinked and coiled.
        const float strength =
            settings.stiffness * std::max((1.0f - share) * (1.0f - share), kTipBend);
        if (strength > 0.0f) {
          keep_apart(at(points_, point), at(points_, point + 2), weights_[std::size_t(point)],
                     weights_[std::size_t(point + 2)], 2.0f * segment_, strength);
        }
      }
      if (settings.ground) {
        for (int point = 1; point < count; ++point) {
          float* low = at(points_, point);
          low[2] = std::max(low[2], ground);
        }
      }
      // The bodies it collides with (the two points at the handle left out: the hand's own arm).
      if (settings.bodies) {
        for (int point = 2; point < count; ++point) {
          for (const LashBody& body : *settings.bodies) {
            push_out(at(points_, point), body);
          }
        }
      }
    }
    // Toward the posed shape: all the way at 1 (its speed then the shape's own movement, which
    // it keeps when the shape lets it go).
    if (pull > 0.0f) {
      posed(handle, along, handle_up, settings, shaped_);
      for (std::size_t part = 3; part < points_.size(); ++part) {
        points_[part] += (shaped_[part] - points_[part]) * pull;
      }
      if (settings.ground) {
        for (int point = 1; point < count; ++point) {
          float* low = at(points_, point);
          low[2] = std::max(low[2], ground);
        }
      }
    }
  }
  for (int axis = 0; axis < 3; ++axis) {
    start_[axis] = start[axis];
    direction_[axis] = direction[axis];
    up_[axis] = up[axis];
  }
}

}  // namespace kcds::runtime::physical_props
