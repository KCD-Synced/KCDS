#include "kcd2s/integration/placed_objects.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

namespace {

PropOrder sample_prop() {
  PropOrder order;
  order.kind = PropKind::Spawn;
  order.id = 4711;
  order.model = "objects/props/barrels/barrel_01.cgf";
  order.position = {10.5f, -3.25f, 200.0f};
  order.rotation = {0.0f, 0.7071068f, 0.0f, 0.7071068f};
  order.scale = {1.0f, 2.0f, 0.5f};
  order.physics = PropPhysics::Rigid;
  order.dimension = 0xffffffffu;
  order.mass = 20.0f;
  return order;
}

VfxOrder sample_vfx(VfxKind kind) {
  VfxOrder order;
  order.kind = kind;
  order.id = 77;
  order.effect = "Library.group.name";
  order.position = {1.0f, 2.0f, 3.0f};
  order.rotation = {0.0f, 0.0f, 1.0f, 0.0f};
  order.dimension = 3;
  order.duration_ms = 2500;
  order.tuning.scale = 2.0f;
  order.tuning.strength = 0.5f;
  order.tuning.pulse_period = 1.5f;
  order.prime = true;
  return order;
}

}  // namespace

KCD2S_TEST(placed_objects, tags_stay_clear_of_other_features) {
  // 69 and 70 are the world time and weather messages of, 71 to 75 the doors' of.
  static_assert(kPropTag == 76);
  static_assert(kVfxTag == 77);
  static_assert(kPropReportTag == 154);
  KCD2S_CHECK(encode_prop(sample_prop()).front() == 76);
  KCD2S_CHECK(encode_vfx(sample_vfx(VfxKind::Spawn)).front() == 77);
}

KCD2S_TEST(placed_objects, prop_round_trips) {
  PropOrder spawn = sample_prop();
  PropOrder decoded;
  KCD2S_CHECK(decode_prop(encode_prop(spawn), decoded));
  KCD2S_CHECK(decoded == spawn);

  PropOrder update;
  update.kind = PropKind::Update;
  update.id = 4711;
  update.scale = {3.0f, 3.0f, 3.0f};
  update.physics = PropPhysics::Static;
  KCD2S_CHECK(decode_prop(encode_prop(update), decoded));
  KCD2S_CHECK(decoded == update);

  PropOrder destroy;
  destroy.kind = PropKind::Destroy;
  destroy.id = 4711;
  KCD2S_CHECK(decode_prop(encode_prop(destroy), decoded));
  KCD2S_CHECK(decoded == destroy);

  // A move: the position and rotation only.
  PropOrder move;
  move.kind = PropKind::Move;
  move.id = 4711;
  move.position = {10.0f, 20.0f, 30.5f};
  move.rotation = {0.0f, 0.0f, 0.7071f, 0.7071f};
  KCD2S_CHECK(decode_prop(encode_prop(move), decoded));
  KCD2S_CHECK(decoded == move);
  // A glide: its time and arc travel; a move without them is one at once.
  PropOrder glide = move;
  glide.glide_ms = 450;
  glide.glide_arc = 0.12f;
  KCD2S_CHECK(decode_prop(encode_prop(glide), decoded));
  KCD2S_CHECK(decoded == glide);
  KCD2S_CHECK(encode_prop(glide).size() > encode_prop(move).size());
  glide.glide_ms = kMaxPropGlideMs + 1;
  KCD2S_CHECK(encode_prop(glide).empty());
  glide.glide_ms = 450;
  glide.glide_arc = kMaxPropGlideArc * 2.0f;
  KCD2S_CHECK(encode_prop(glide).empty());
  move.position[0] = INFINITY;
  KCD2S_CHECK(encode_prop(move).empty());
}

// A prop on a body: the body and joint travel; it has no physics, a joint needs a
// body, and its offset stays within reach.
KCD2S_TEST(placed_objects, prop_on_a_body_round_trips) {
  PropOrder hung = sample_prop();
  hung.physics = PropPhysics::None;
  hung.body = 12;
  hung.joint = "Bip01 Head";
  hung.position = {0.0f, 0.1f, 0.2f};
  PropOrder decoded;
  KCD2S_CHECK(decode_prop(encode_prop(hung), decoded));
  KCD2S_CHECK(decoded == hung);
  hung.joint.clear();
  KCD2S_CHECK(decode_prop(encode_prop(hung), decoded) && decoded == hung);
  PropOrder solid = hung;
  solid.physics = PropPhysics::Static;
  KCD2S_CHECK(encode_prop(solid).empty());
  PropOrder far = hung;
  far.position[2] = kMaxPropOffset + 1.0f;
  KCD2S_CHECK(encode_prop(far).empty());
  PropOrder loose_joint = sample_prop();
  loose_joint.joint = "Bip01 Head";
  KCD2S_CHECK(encode_prop(loose_joint).empty());
  // An order from a server before (no body, no joint) still reads.
  PropOrder plain = sample_prop();
  auto bytes = encode_prop(plain);
  bytes.resize(bytes.size() - 2);
  KCD2S_CHECK(decode_prop(bytes, decoded) && decoded == plain);
}

// A rigid prop's simulation: who simulates it, the pushes, and its reports.
KCD2S_TEST(placed_objects, prop_simulation_round_trips) {
  PropOrder decoded;
  PropOrder simulate;
  simulate.kind = PropKind::Simulate;
  simulate.id = 4711;
  simulate.simulate = true;
  KCD2S_CHECK(decode_prop(encode_prop(simulate), decoded));
  KCD2S_CHECK(decoded == simulate);

  PropOrder push;
  push.kind = PropKind::Push;
  push.id = 4711;
  push.push = PropPush::Impulse;
  push.linear = {0.0f, 0.0f, 150.0f};
  push.angular = {1.0f, 0.0f, 0.0f};
  push.has_point = true;
  push.point = {10.0f, 20.0f, 30.0f};
  KCD2S_CHECK(decode_prop(encode_prop(push), decoded));
  KCD2S_CHECK(decoded == push);
  push.push = PropPush::Sleep;
  push.has_point = false;
  push.point = {};
  KCD2S_CHECK(decode_prop(encode_prop(push), decoded));
  KCD2S_CHECK(decoded == push);
  push.linear[2] = NAN;
  KCD2S_CHECK(encode_prop(push).empty());
  push.linear[2] = 0.0f;
  push.push = PropPush(9);
  KCD2S_CHECK(encode_prop(push).empty());

  // The mass travels with a spawn and an update; 0 to 100,000 kg.
  PropOrder heavy = sample_prop();
  heavy.mass = 100000.0f;
  KCD2S_CHECK(decode_prop(encode_prop(heavy), decoded));
  KCD2S_CHECK(decoded.mass == 100000.0f);
  heavy.mass = 100001.0f;
  KCD2S_CHECK(encode_prop(heavy).empty());
  heavy.mass = -1.0f;
  KCD2S_CHECK(encode_prop(heavy).empty());

  PropReport report;
  report.id = 4711;
  report.position = {1.0f, 2.0f, 3.0f};
  report.rotation = {0.0f, 0.0f, 0.7071f, 0.7071f};
  report.resting = true;
  PropReport read;
  KCD2S_CHECK(decode_prop_report(encode_prop_report(report), read));
  KCD2S_CHECK(read == report);
  report.id = 0;
  KCD2S_CHECK(encode_prop_report(report).empty());
  report.id = 4711;
  report.rotation[3] = INFINITY;
  KCD2S_CHECK(encode_prop_report(report).empty());
  KCD2S_CHECK(!decode_prop_report({kPropReportTag, 1}, read));
  KCD2S_CHECK(!decode_prop_report({kPropTag, 1, 1}, read));
  // A hit: its speed travels; a speed out of range does not; a report without it
  // (an older client) has none.
  report.rotation[3] = 0.7071f;
  report.hit_speed = 7.5f;
  KCD2S_CHECK(decode_prop_report(encode_prop_report(report), read) && read == report);
  report.hit_speed = -1.0f;
  KCD2S_CHECK(encode_prop_report(report).empty());
  report.hit_speed = 0.0f;
  auto older = encode_prop_report(report);
  older.resize(older.size() - 4);
  KCD2S_CHECK(decode_prop_report(older, read) && read == report);
}

KCD2S_TEST(placed_objects, prop_refuses_bad_orders) {
  PropOrder order = sample_prop();
  order.model.clear();
  KCD2S_CHECK(encode_prop(order).empty());
  order = sample_prop();
  order.position[1] = INFINITY;
  KCD2S_CHECK(encode_prop(order).empty());
  PropOrder decoded;
  KCD2S_CHECK(!decode_prop({kPropTag, 9, 1}, decoded));
  KCD2S_CHECK(!decode_prop({kPropTag, std::uint8_t(PropKind::Destroy), 0}, decoded));
  KCD2S_CHECK(!decode_prop({kVfxTag, 1, 1}, decoded));
  auto cut = encode_prop(sample_prop());
  cut.resize(cut.size() - 3);
  KCD2S_CHECK(!decode_prop(cut, decoded));
}

KCD2S_TEST(placed_objects, vfx_round_trips) {
  VfxOrder decoded;
  for (const VfxKind kind : {VfxKind::Spawn, VfxKind::Burst, VfxKind::BurstOn}) {
    VfxOrder order = sample_vfx(kind);
    if (kind == VfxKind::Burst) {
      order.id = 0;
    }
    if (kind == VfxKind::BurstOn) {
      order.dimension = 0;
    }
    if (kind == VfxKind::Spawn) {
      order.duration_ms = 0;
    }
    KCD2S_CHECK(decode_vfx(encode_vfx(order), decoded));
    KCD2S_CHECK(decoded == order);
  }

  VfxOrder update;
  update.kind = VfxKind::Update;
  update.id = 77;
  update.tuning.speed_scale = 4.0f;
  KCD2S_CHECK(decode_vfx(encode_vfx(update), decoded));
  KCD2S_CHECK(decoded == update);

  VfxOrder destroy;
  destroy.kind = VfxKind::Destroy;
  destroy.id = 77;
  KCD2S_CHECK(decode_vfx(encode_vfx(destroy), decoded));
  KCD2S_CHECK(decoded == destroy);

  // A burst on a body's joint.
  VfxOrder on_joint = sample_vfx(VfxKind::BurstOn);
  on_joint.dimension = 0;
  on_joint.joint = "Bip01 R Hand";
  KCD2S_CHECK(decode_vfx(encode_vfx(on_joint), decoded));
  KCD2S_CHECK(decoded == on_joint);
  KCD2S_CHECK(decoded.joint == "Bip01 R Hand");

  // An effect on a body's joint; one from a server before it hangs on nothing.
  VfxOrder on_body = sample_vfx(VfxKind::Spawn);
  on_body.duration_ms = 0;
  on_body.body = 9;
  on_body.joint = "RightHand";
  KCD2S_CHECK(decode_vfx(encode_vfx(on_body), decoded));
  KCD2S_CHECK(decoded == on_body);
  VfxOrder plain = sample_vfx(VfxKind::Spawn);
  plain.duration_ms = 0;
  auto older = encode_vfx(plain);
  older.resize(older.size() - 2);
  KCD2S_CHECK(decode_vfx(older, decoded) && decoded == plain);
}

KCD2S_TEST(placed_objects, vfx_refuses_bad_orders) {
  VfxOrder order = sample_vfx(VfxKind::Burst);
  order.duration_ms = kMaxBurstMilliseconds + 1;
  KCD2S_CHECK(encode_vfx(order).empty());
  order = sample_vfx(VfxKind::Spawn);
  order.effect.clear();
  KCD2S_CHECK(encode_vfx(order).empty());
  order = sample_vfx(VfxKind::Spawn);
  order.tuning.strength = 2.0f;
  KCD2S_CHECK(encode_vfx(order).empty());
  order = sample_vfx(VfxKind::BurstOn);
  order.joint = "Right\nHand";
  KCD2S_CHECK(encode_vfx(order).empty());
  order.joint = std::string(kMaxVfxJointBytes + 1, 'j');
  KCD2S_CHECK(encode_vfx(order).empty());
  VfxOrder decoded;
  KCD2S_CHECK(!decode_vfx({kVfxTag, 0, 1}, decoded));
  KCD2S_CHECK(!decode_vfx({kPropTag, 1, 1}, decoded));
}
