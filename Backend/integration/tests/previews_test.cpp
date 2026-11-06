#include "kcd2s/integration/gizmos.hpp"
#include "kcd2s/integration/previews.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

namespace {

std::vector<PreviewPiece> sample_pieces() {
  PreviewPiece barrel;
  barrel.model = "objects/props/barrels/barrel_01.cgf";
  PreviewPiece lid;
  lid.model = "objects/props/barrels/barrel_lid.cgf";
  lid.offset = {0.0f, 0.0f, 1.1f};
  lid.rotation = {0.0f, 0.0f, 0.7071068f, 0.7071068f};
  return {barrel, lid};
}

GhostOrder sample_ghost() {
  GhostOrder order;
  order.kind = GhostKind::Create;
  order.ghost = 12;
  order.pieces = sample_pieces();
  order.position = {100.0f, -20.5f, 33.0f};
  order.rotation = {0.0f, 0.0f, 1.0f, 0.0f};
  order.scale = 1.5f;
  order.tint.present = true;
  order.tint.rgba = {0.0f, 1.0f, 0.0f, 0.5f};
  order.hidden = true;
  return order;
}

PlacerOrder sample_begin() {
  PlacerOrder order;
  order.session = 3;
  order.pieces = sample_pieces();
  order.scale = 2.0f;
  order.range = 12.0f;
  order.fallback_distance = 4.0f;
  order.vertical_offset = 0.25f;
  order.yaw = 90.0f;
  order.flags = kPlacerAlignToSurface | kPlacerKeepOpen | kPlacerReportMoves;
  order.max_slope = 30.0f;
  order.max_distance = 8.0f;
  return order;
}

}  // namespace

KCD2S_TEST(previews, tags_stay_in_the_0_2_1_range) {
  // 100 to 105 belong to the previews and gizmos; 106 is the 0.2.0 NPC_LOOK message.
  static_assert(kGhostTag == 100);
  static_assert(kPlacerTag == 101);
  static_assert(kPlacerReportTag == 102);
  static_assert(kGizmoTag == 103);
  KCD2S_CHECK(encode_ghost(sample_ghost()).front() == 100);
  KCD2S_CHECK(encode_placer(sample_begin()).front() == 101);
}

KCD2S_TEST(previews, ghost_round_trips) {
  GhostOrder decoded;
  const GhostOrder create = sample_ghost();
  KCD2S_CHECK(decode_ghost(encode_ghost(create), decoded));
  KCD2S_CHECK(decoded == create);

  GhostOrder pose;
  pose.kind = GhostKind::Pose;
  pose.ghost = 12;
  pose.position = {1.0f, 2.0f, 3.0f};
  pose.scale = 0.5f;
  KCD2S_CHECK(decode_ghost(encode_ghost(pose), decoded));
  KCD2S_CHECK(decoded == pose);

  GhostOrder untint;
  untint.kind = GhostKind::Tint;
  untint.ghost = 12;
  KCD2S_CHECK(decode_ghost(encode_ghost(untint), decoded));
  KCD2S_CHECK(decoded == untint);
  KCD2S_CHECK(!decoded.tint.present);

  GhostOrder destroy;
  destroy.kind = GhostKind::Destroy;
  destroy.ghost = 12;
  KCD2S_CHECK(decode_ghost(encode_ghost(destroy), decoded));
  KCD2S_CHECK(decoded == destroy);
}

// a scale per axis travels after the other fields; without one the message is the
// one a 0.3.4 peer sends, and a 0.3.4 peer reads the fields before it.
KCD2S_TEST(previews, ghost_axes_round_trip) {
  GhostOrder create = sample_ghost();
  create.axes = {1.9f, 1.5f, 0.7f};
  GhostOrder decoded;
  KCD2S_CHECK(decode_ghost(encode_ghost(create), decoded));
  KCD2S_CHECK(decoded == create);

  GhostOrder pose;
  pose.kind = GhostKind::Pose;
  pose.ghost = 12;
  pose.axes = {2.0f, 1.0f, 0.5f};
  KCD2S_CHECK(decode_ghost(encode_ghost(pose), decoded));
  KCD2S_CHECK(decoded == pose);

  const GhostOrder uniform = sample_ghost();
  const std::vector<std::uint8_t> plain = encode_ghost(uniform);
  std::vector<std::uint8_t> scaled = encode_ghost(create);
  KCD2S_CHECK(scaled.size() > plain.size());
  KCD2S_CHECK(decode_ghost(plain, decoded));
  KCD2S_CHECK(decoded.axes == (std::array<float, 3>{1.0f, 1.0f, 1.0f}));

  create.axes = {0.0f, 1.0f, 1.0f};
  KCD2S_CHECK(encode_ghost(create).empty());
}

KCD2S_TEST(previews, bad_ghosts_do_not_travel) {
  GhostOrder no_pieces = sample_ghost();
  no_pieces.pieces.clear();
  KCD2S_CHECK(encode_ghost(no_pieces).empty());
  GhostOrder bright = sample_ghost();
  bright.tint.rgba[3] = 2.0f;
  KCD2S_CHECK(encode_ghost(bright).empty());
  GhostOrder huge = sample_ghost();
  huge.scale = 1000.0f;
  KCD2S_CHECK(encode_ghost(huge).empty());
  GhostOrder nobody = sample_ghost();
  nobody.ghost = 0;
  KCD2S_CHECK(encode_ghost(nobody).empty());
  auto truncated = encode_ghost(sample_ghost());
  truncated.resize(truncated.size() / 2);
  GhostOrder decoded;
  KCD2S_CHECK(!decode_ghost(truncated, decoded));
}

KCD2S_TEST(previews, placer_round_trips) {
  PlacerOrder decoded;
  const PlacerOrder begin = sample_begin();
  KCD2S_CHECK(decode_placer(encode_placer(begin), decoded));
  KCD2S_CHECK(decoded == begin);

  PlacerOrder veto;
  veto.kind = PlacerKind::Veto;
  veto.session = 3;
  veto.vetoed = true;
  KCD2S_CHECK(decode_placer(encode_placer(veto), decoded));
  KCD2S_CHECK(decoded == veto);

  PlacerOrder far = sample_begin();
  far.range = 600.0f;
  KCD2S_CHECK(encode_placer(far).empty());
}

KCD2S_TEST(previews, placer_report_round_trips) {
  PlacerReport confirm;
  confirm.event = PlacerEvent::Confirm;
  confirm.session = 3;
  confirm.valid = true;
  confirm.position = {5.0f, 6.0f, 7.0f};
  confirm.rotation = {0.0f, 0.0f, 0.3826834f, 0.9238795f};
  confirm.normal = {0.0f, 0.1f, 0.99f};
  confirm.distance = 4.5f;
  confirm.on_surface = true;
  PlacerReport decoded;
  KCD2S_CHECK(decode_placer_report(encode_placer_report(confirm), decoded));
  KCD2S_CHECK(decoded == confirm);

  PlacerReport end;
  end.event = PlacerEvent::End;
  end.session = 3;
  const auto bytes = encode_placer_report(end);
  KCD2S_CHECK(bytes.size() < 6);
  KCD2S_CHECK(decode_placer_report(bytes, decoded));
  KCD2S_CHECK(decoded == end);

  PlacerReport unknown = end;
  unknown.event = PlacerEvent(9);
  KCD2S_CHECK(encode_placer_report(unknown).empty());
}

KCD2S_TEST(gizmos, gizmo_round_trips) {
  GizmoOrder text;
  text.gizmo = 9;
  text.shape = GizmoShape::Text;
  text.position = {1.0f, 2.0f, 3.0f};
  text.size = 1.5f;
  text.color = {1.0f, 0.5f, 0.0f, 1.0f};
  text.duration_ms = 5000;
  text.text = "spawn point";
  GizmoOrder decoded;
  KCD2S_CHECK(encode_gizmo(text).front() == 103);
  KCD2S_CHECK(decode_gizmo(encode_gizmo(text), decoded));
  KCD2S_CHECK(decoded == text);

  GizmoOrder box;
  box.gizmo = 10;
  box.shape = GizmoShape::Box;
  box.extent = {0.5f, 1.0f, 2.0f};
  box.rotation = {0.0f, 0.0f, 0.7071068f, 0.7071068f};
  KCD2S_CHECK(decode_gizmo(encode_gizmo(box), decoded));
  KCD2S_CHECK(decoded == box);

  GizmoOrder clear;
  clear.kind = GizmoKind::ClearAll;
  KCD2S_CHECK(decode_gizmo(encode_gizmo(clear), decoded));
  KCD2S_CHECK(decoded == clear);
}

KCD2S_TEST(gizmos, bad_gizmos_do_not_travel) {
  GizmoOrder line;
  line.gizmo = 1;
  line.shape = GizmoShape::Line;
  line.text = "a line has no text";
  KCD2S_CHECK(encode_gizmo(line).empty());
  GizmoOrder silent;
  silent.gizmo = 2;
  silent.shape = GizmoShape::Text;
  KCD2S_CHECK(encode_gizmo(silent).empty());
  GizmoOrder forever;
  forever.gizmo = 3;
  forever.duration_ms = kMaxGizmoMilliseconds + 1;
  KCD2S_CHECK(encode_gizmo(forever).empty());
  GizmoOrder shapeless;
  shapeless.gizmo = 4;
  shapeless.shape = GizmoShape(0);
  KCD2S_CHECK(encode_gizmo(shapeless).empty());
}

// A pose replaces the ghost's waiting pose; a full queue drops a pose, never a destroy.
KCD2S_TEST(previews, ghost_queue_keeps_destroys_and_the_newest_pose) {
  const auto order = [](GhostKind kind, std::uint32_t ghost, float x) {
    GhostOrder made;
    made.kind = kind;
    made.ghost = ghost;
    made.position = {x, 0.0f, 0.0f};
    return made;
  };
  std::vector<GhostOrder> queue;
  queue_ghost_order(queue, order(GhostKind::Create, 1, 0.0f), 4);
  for (int step = 1; step <= 50; ++step) {
    queue_ghost_order(queue, order(GhostKind::Pose, 1, float(step)), 4);
  }
  KCD2S_CHECK_EQ(queue.size(), std::size_t(2));
  KCD2S_CHECK(queue[1].kind == GhostKind::Pose && queue[1].position[0] == 50.0f);
  // Full of other ghosts' poses: a destroy still gets in, a pose makes room.
  queue_ghost_order(queue, order(GhostKind::Pose, 2, 1.0f), 4);
  queue_ghost_order(queue, order(GhostKind::Pose, 3, 1.0f), 4);
  KCD2S_CHECK_EQ(queue.size(), std::size_t(4));
  queue_ghost_order(queue, order(GhostKind::Destroy, 1, 0.0f), 4);
  KCD2S_CHECK_EQ(queue.size(), std::size_t(4));
  KCD2S_CHECK(queue.back().kind == GhostKind::Destroy && queue.back().ghost == 1);
  KCD2S_CHECK(queue.front().kind == GhostKind::Create);
  // Only creates and destroys left: a new one waits for the next frame's room.
  std::vector<GhostOrder> full(4, order(GhostKind::Destroy, 9, 0.0f));
  queue_ghost_order(full, order(GhostKind::Create, 10, 0.0f), 4);
  KCD2S_CHECK_EQ(full.size(), std::size_t(4));
}
