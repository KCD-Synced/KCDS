#include "motion_index.hpp"

#include <array>

#include "kcd2s/testkit.hpp"

namespace {
struct Motion {
  void* animated = nullptr;
  unsigned sequence = 0;
};
using kcds::runtime::replicas::MotionIndex;
}  // namespace

KCD2S_TEST(motion_index, model_replacement_and_pointer_reuse_retire_old_identity) {
  MotionIndex<Motion> index;
  int old_skeleton, new_skeleton, old_animated, new_animated;
  index.assign(&old_skeleton, {&old_animated, 1});
  index.assign(&new_skeleton, {&old_animated, 2});
  KCD2S_CHECK(index.size() == 1);
  KCD2S_CHECK(index.find(&old_skeleton) == index.end());
  KCD2S_CHECK(index.by_animated(&old_animated)->sequence == 2);
  index.erase(&old_skeleton);
  KCD2S_CHECK(index.by_animated(&old_animated)->sequence == 2);

  index.assign(&new_skeleton, {&new_animated, 3});
  KCD2S_CHECK(index.by_animated(&old_animated) == nullptr);
  KCD2S_CHECK(index.by_animated(&new_animated)->sequence == 3);
  index.erase(&new_skeleton);
  KCD2S_CHECK(index.size() == 0);
  KCD2S_CHECK(index.by_animated(&new_animated) == nullptr);
}

KCD2S_TEST(motion_index, dense_character_churn_keeps_only_live_bodies) {
  MotionIndex<Motion> index;
  std::array<int, 1024> skeletons{};
  std::array<int, 1024> animated{};
  for (unsigned round = 1; round <= 10; ++round) {
    for (unsigned i = 0; i < skeletons.size(); ++i) {
      index.assign(&skeletons[i], {&animated[i], round});
    }
    for (unsigned i = 0; i < skeletons.size(); i += 2) {
      index.erase(&skeletons[i]);
    }
    KCD2S_CHECK(index.size() == skeletons.size() / 2);
    for (unsigned i = 0; i < skeletons.size(); ++i) {
      const auto* state = index.by_animated(&animated[i]);
      KCD2S_CHECK(i % 2 ? state && state->sequence == round : !state);
    }
    index.clear();
    for (auto& body : animated) {
      KCD2S_CHECK(index.by_animated(&body) == nullptr);
    }
  }
}

KCD2S_TEST(motion_index, animation_only_state_has_no_null_movement_identity) {
  MotionIndex<Motion> index;
  int skeleton;
  index.assign(&skeleton, {nullptr, 1});
  KCD2S_CHECK(index.find(&skeleton) != index.end());
  KCD2S_CHECK(index.by_animated(nullptr) == nullptr);
  index.clear();
  KCD2S_CHECK(index.size() == 0);
}
