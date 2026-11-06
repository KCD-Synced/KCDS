#include "paired_start.hpp"
#include "kcd2s/testkit.hpp"

#include <cmath>

using namespace kcds::runtime::animation::paired_start;

namespace {

constexpr float kPi = 3.14159265f;

Place at(float x, float y, float z, float yaw) {
  Place place;
  place.q[2] = std::sin(yaw * 0.5f);
  place.q[3] = std::cos(yaw * 0.5f);
  place.t[0] = x;
  place.t[1] = y;
  place.t[2] = z;
  return place;
}

bool near(float a, float b) {
  return std::fabs(a - b) < 1e-4f;
}

float yaw_of(const Place& place) {
  return 2.0f * std::atan2(place.q[2], place.q[3]);
}

bool same_place(const Place& place, float x, float y, float z, float yaw) {
  float off = std::fmod(yaw_of(place) - yaw + 3.0f * kPi, 2.0f * kPi) - kPi;
  return near(place.t[0], x) && near(place.t[1], y) && near(place.t[2], z) && near(off, 0.0f);
}

}  // namespace

KCD2S_TEST(runtime_paired_start, puts_the_slave_where_its_clip_starts_from_the_master) {
  // The clips' scene: the master at its origin, the victim 0.8 m in front of it (+y), facing the
  // same way.
  const Place master_start = at(0.0f, 0.0f, 0.0f, 0.0f);
  const Place slave_start = at(0.0f, 0.8f, 0.0f, 0.0f);
  // The master stands at (100, 200, 30) facing west (+90 degrees: forward is -x).
  const Place master = at(100.0f, 200.0f, 30.0f, kPi / 2.0f);
  KCD2S_CHECK(
      same_place(slave_place(master, master_start, slave_start), 99.2f, 200.0f, 30.0f, kPi / 2.0f));
}

KCD2S_TEST(runtime_paired_start, takes_the_clips_own_origin_and_turns_out) {
  // Exported away from the origin and turned: the master at (5, 5) facing +x (-90 degrees), the
  // victim 1 m ahead of it (6, 5) facing the master (+90 degrees).
  const Place master_start = at(5.0f, 5.0f, 0.0f, -kPi / 2.0f);
  const Place slave_start = at(6.0f, 5.0f, 0.0f, kPi / 2.0f);
  // The master at (10, 20, 1) facing north (0): the victim 1 m north of it, facing south.
  const Place master = at(10.0f, 20.0f, 1.0f, 0.0f);
  KCD2S_CHECK(same_place(slave_place(master, master_start, slave_start), 10.0f, 21.0f, 1.0f, kPi));
}

KCD2S_TEST(runtime_paired_start, keeps_the_body_upright) {
  // A start location tilted 30 degrees about x keeps only its heading.
  Place tilted = at(0.0f, 1.0f, 0.0f, 0.0f);
  tilted.q[0] = std::sin(kPi / 12.0f);
  tilted.q[3] = std::cos(kPi / 12.0f);
  const Place place = slave_place(at(0.0f, 0.0f, 0.0f, 0.0f), at(0.0f, 0.0f, 0.0f, 0.0f), tilted);
  KCD2S_CHECK(near(place.q[0], 0.0f));
  KCD2S_CHECK(near(place.q[1], 0.0f));
  KCD2S_CHECK(same_place(place, 0.0f, 1.0f, 0.0f, 0.0f));
}

KCD2S_TEST(runtime_paired_start, inverse_undoes_a_place) {
  const Place place = at(3.0f, -2.0f, 7.0f, 1.1f);
  KCD2S_CHECK(same_place(compose(place, inverse(place)), 0.0f, 0.0f, 0.0f, 0.0f));
  KCD2S_CHECK(same_place(compose(inverse(place), place), 0.0f, 0.0f, 0.0f, 0.0f));
}

KCD2S_TEST(runtime_paired_start, has_no_location_without_a_set_or_a_clip) {
  Place place;
  KCD2S_CHECK(!start_location(nullptr, 3, place));
}
