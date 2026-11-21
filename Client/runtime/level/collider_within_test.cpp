// The level's invisible colliders that go with a deleted or moved object.
#include "collider_within.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::level::collider_within;

namespace {

// A yard gate: 3 m wide, 0.4 m deep, 2 m tall, standing on the ground at z 100.
const float kGateMin[3] = {10.0f, 20.0f, 100.0f};
const float kGateMax[3] = {13.0f, 20.4f, 102.0f};

}  // namespace

KCD2S_TEST(collider_within, a_gates_own_collider_goes_with_it) {
  // The gate's collider, a box a little apart from the gate's place, a little wider than its mesh.
  const float own[6] = {9.9f, 20.05f, 100.0f, 13.2f, 20.35f, 101.8f};
  KCD2S_CHECK(collider_within(own, kGateMin, kGateMax));
}

KCD2S_TEST(collider_within, a_wall_reaching_past_it_stays) {
  // The yard wall's collider runs on past the gateway.
  const float wall[6] = {0.0f, 20.0f, 100.0f, 30.0f, 20.4f, 102.0f};
  KCD2S_CHECK(!collider_within(wall, kGateMin, kGateMax));
}

KCD2S_TEST(collider_within, a_floor_at_its_feet_stays) {
  // A castle gateway's floor over the ground's hole: flat, at the gate's bottom.
  const float floor[6] = {10.0f, 20.0f, 99.8f, 13.0f, 20.4f, 100.1f};
  KCD2S_CHECK(!collider_within(floor, kGateMin, kGateMax));
  // A low step inside its bounds is a floor too.
  const float step[6] = {10.5f, 20.1f, 100.0f, 12.5f, 20.3f, 100.3f};
  KCD2S_CHECK(!collider_within(step, kGateMin, kGateMax));
}

KCD2S_TEST(collider_within, within_the_margin_only) {
  const float edge[6] = {9.75f, 20.0f, 100.0f, 13.0f, 20.4f, 102.0f};
  KCD2S_CHECK(collider_within(edge, kGateMin, kGateMax));
  const float past[6] = {9.6f, 20.0f, 100.0f, 13.0f, 20.4f, 102.0f};
  KCD2S_CHECK(!collider_within(past, kGateMin, kGateMax));
}
