#include "kcd2s/bridge/apply.hpp"

#include <algorithm>

#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/snapshot.hpp"
#include "kcd2s/replication/world.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s::replication;
using kcd2s::bridge::extract_transforms;
using kcd2s::bridge::TransformApply;
using kcd2s::core::Quat;
using kcd2s::core::Vec3;

// Build a mirror by replicating a small world (exercises the real snapshot path into the bridge).
KCD2S_TEST(apply, extracts_transforms_from_mirror) {
  SchemaRegistry reg;
  reg.define("Human")
      .field("position", FieldKind::Vec3, Priority::High)
      .field("rotation", FieldKind::Quat, Priority::High)
      .field("health", FieldKind::F32, Priority::Med);
  const auto human = reg.get("Human")->id;

  World w(reg);
  w.set_tick(1);
  const auto a = w.spawn(human);
  w.write(a, 0, Vec3{10, 20, 30});
  w.write(a, 1, Quat{0, 0, 0, 1});
  w.write(a, 2, 0.5f);

  Mirror mirror(reg);
  ClientRepl client;
  mirror.apply(build_snapshot(w, 1, {a}, client, nullptr));

  auto applies = extract_transforms(reg, mirror);
  KCD2S_CHECK_EQ(applies.size(), std::size_t{1});
  const TransformApply& t = applies[0];
  KCD2S_CHECK_EQ(t.entity, a);
  KCD2S_CHECK(t.has_position);
  KCD2S_CHECK(t.has_rotation);
  KCD2S_CHECK((t.position == Vec3{10, 20, 30}));
  KCD2S_CHECK((t.rotation == Quat{0, 0, 0, 1}));
}

KCD2S_TEST(apply, type_without_transform_skipped) {
  SchemaRegistry reg;
  reg.define("Door").field("locked", FieldKind::Bool, Priority::Low);
  const auto door = reg.get("Door")->id;

  World w(reg);
  w.set_tick(1);
  const auto d = w.spawn(door);
  w.write(d, 0, true);

  Mirror mirror(reg);
  ClientRepl client;
  mirror.apply(build_snapshot(w, 1, {d}, client, nullptr));

  auto applies = extract_transforms(reg, mirror);
  KCD2S_CHECK_EQ(applies.size(), std::size_t{0});  // no position/rotation fields
}

KCD2S_TEST(apply, position_only_type) {
  SchemaRegistry reg;
  reg.define("Prop").field("position", FieldKind::Vec3, Priority::Med);
  const auto prop = reg.get("Prop")->id;

  World w(reg);
  w.set_tick(1);
  const auto p = w.spawn(prop);
  w.write(p, 0, Vec3{1, 2, 3});

  Mirror mirror(reg);
  ClientRepl client;
  mirror.apply(build_snapshot(w, 1, {p}, client, nullptr));

  auto applies = extract_transforms(reg, mirror);
  KCD2S_CHECK_EQ(applies.size(), std::size_t{1});
  KCD2S_CHECK(applies[0].has_position);
  KCD2S_CHECK(!applies[0].has_rotation);
}
