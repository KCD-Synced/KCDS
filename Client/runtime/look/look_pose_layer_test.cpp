#include "look_pose_layer.hpp"

#include "kcd2s/testkit.hpp"

namespace {
using kcds::runtime::look::kLookPoseLayer;
using kcds::runtime::look::owner_clip_step;
using kcds::runtime::look::OwnerClipStep;

// One frame of the replay over two layers, as the replica loops do it: a consumed or played clip
// is taken, a waiting one stays for a later frame.
struct Layer {
  unsigned layer = 0;
  std::uint64_t sequence = 0;
  std::uint64_t applied = 0;
  unsigned played = 0;
};

void frame(Layer& layer, bool native_window) {
  const OwnerClipStep step =
      owner_clip_step(layer.layer, layer.sequence, layer.applied, native_window);
  if (step == OwnerClipStep::Wait || step == OwnerClipStep::Done) {
    return;
  }
  if (step == OwnerClipStep::Play) {
    ++layer.played;
  }
  layer.applied = layer.sequence;
}
}  // namespace

KCD2S_TEST(look_pose_layer, the_look_pose_layer_is_the_human_look_pose_scope) {
  KCD2S_CHECK_EQ(kLookPoseLayer, 15u);
}

KCD2S_TEST(look_pose_layer, outside_a_native_window_a_newer_clip_plays_on_every_layer) {
  KCD2S_CHECK(owner_clip_step(0, 5, 4, false) == OwnerClipStep::Play);
  KCD2S_CHECK(owner_clip_step(kLookPoseLayer, 5, 4, false) == OwnerClipStep::Play);
  KCD2S_CHECK(owner_clip_step(kLookPoseLayer, 5, 5, false) == OwnerClipStep::Done);
  KCD2S_CHECK(owner_clip_step(kLookPoseLayer, 0, 0, false) == OwnerClipStep::Done);
}

KCD2S_TEST(look_pose_layer, in_a_native_window_only_the_look_pose_waits) {
  KCD2S_CHECK(owner_clip_step(0, 5, 4, true) == OwnerClipStep::Consume);
  KCD2S_CHECK(owner_clip_step(14, 5, 4, true) == OwnerClipStep::Consume);
  KCD2S_CHECK(owner_clip_step(kLookPoseLayer, 5, 4, true) == OwnerClipStep::Wait);
  KCD2S_CHECK(owner_clip_step(kLookPoseLayer, 4, 4, true) == OwnerClipStep::Done);
}

KCD2S_TEST(look_pose_layer, the_riding_look_pose_started_in_the_mount_plays_once_it_is_over) {
  // The owner's mount starts the riding look poses on layer 15 and its mount clip on layer 0.
  Layer look{kLookPoseLayer, 37, 15};
  Layer body{0, 36, 30};
  for (unsigned mounting = 0; mounting < 40; ++mounting) {
    frame(look, true);
    frame(body, true);
  }
  KCD2S_CHECK_EQ(look.played, 0u);
  KCD2S_CHECK_EQ(look.applied, std::uint64_t(15));
  KCD2S_CHECK_EQ(body.played, 0u);
  KCD2S_CHECK_EQ(body.applied, std::uint64_t(36));
  // Seated: the look poses play once, the consumed mount clip never.
  frame(look, false);
  frame(body, false);
  frame(look, false);
  KCD2S_CHECK_EQ(look.played, 1u);
  KCD2S_CHECK_EQ(look.applied, std::uint64_t(37));
  KCD2S_CHECK_EQ(body.played, 0u);
}

KCD2S_TEST(look_pose_layer, a_newer_look_pose_in_the_window_replaces_the_waiting_one) {
  // The owner dismounts: its standing look poses start while the replica still dismounts.
  Layer look{kLookPoseLayer, 50, 47};
  frame(look, true);
  look.sequence = 52;
  frame(look, true);
  frame(look, false);
  KCD2S_CHECK_EQ(look.played, 1u);
  KCD2S_CHECK_EQ(look.applied, std::uint64_t(52));
}
