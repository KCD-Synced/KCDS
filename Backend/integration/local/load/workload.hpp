#pragma once
// Deterministic synthetic movement for kcd2s-load-probe. Every player runs a circle at a constant
// speed; only the placement of the circles differs between workloads:
//   distributed - each player circles its own centre on a square grid (`spacing` apart),
//   dense       - every player circles the origin inside `dense_radius`, so all share one area.
// Horizontal plane is x/y, z is height (as in the network probe). Portable and unit tested.
#include <cmath>
#include <cstddef>

#include "kcd2s/core/math.hpp"
#include "load_options.hpp"

namespace kcd2s::local::load {

struct MovementSample {
  core::Vec3 position;
  core::Vec3 velocity;
  core::Quat rotation;  // yaw about +z, facing the direction of travel
};

struct CirclePath {
  core::Vec3 centre;
  float radius = 1.0f;
  float phase = 0.0f;  // radians at t = 0
};

inline constexpr float kGoldenAngle = 2.39996323f;

inline CirclePath circle_for(const LoadOptions& options, std::size_t index, std::size_t count) {
  CirclePath path;
  path.phase = std::fmod(static_cast<float>(index) * kGoldenAngle, 6.28318531f);
  if (options.workload == Workload::Dense) {
    // Spread the circle radii over [20%, 100%] of the dense radius with a low-discrepancy step.
    const float fraction = std::fmod(static_cast<float>(index) * 0.61803399f, 1.0f);
    path.radius = options.dense_radius_meters * (0.2f + 0.8f * fraction);
    return path;
  }
  const auto side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(count))));
  const float offset = static_cast<float>(side - 1) / 2.0f;
  const float column = static_cast<float>(index % side);
  const float row = static_cast<float>(index / side);
  path.centre = core::Vec3{(column - offset) * options.spacing_meters,
                           (row - offset) * options.spacing_meters, 0.0f};
  path.radius = options.move_radius_meters;
  return path;
}

inline MovementSample sample_movement(const CirclePath& path, float speed, double seconds) {
  const float angular_speed = speed / path.radius;
  const float angle =
      path.phase + static_cast<float>(std::fmod(seconds * angular_speed, 6.283185307179586));
  const float cosine = std::cos(angle);
  const float sine = std::sin(angle);
  MovementSample sample;
  sample.position = core::Vec3{path.centre.x + path.radius * cosine,
                               path.centre.y + path.radius * sine, path.centre.z};
  sample.velocity = core::Vec3{-speed * sine, speed * cosine, 0.0f};
  const float yaw = std::atan2(sample.velocity.y, sample.velocity.x);
  sample.rotation = core::Quat{0.0f, 0.0f, std::sin(yaw / 2.0f), std::cos(yaw / 2.0f)};
  return sample;
}

}  // namespace kcd2s::local::load
