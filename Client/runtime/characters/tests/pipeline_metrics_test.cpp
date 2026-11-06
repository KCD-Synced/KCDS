#include "../pipeline_metrics_sample.hpp"

#include <limits>

#include "kcd2s/testkit.hpp"

using kcds::runtime::characters::pipeline_metrics::AnimationSpan;
using kcds::runtime::characters::pipeline_metrics::PostChild;
using kcds::runtime::characters::pipeline_metrics::PostDetailSample;
using kcds::runtime::characters::pipeline_metrics::PostDetailWindow;
using kcds::runtime::characters::pipeline_metrics::ThreadCpuStamp;
using kcds::runtime::characters::pipeline_metrics::TimingWindow;

KCD2S_TEST(pipeline_metrics, split_measures_lock_and_overlay_separately_from_present) {
  TimingWindow window;
  KCD2S_CHECK(window.add(1000, 1010, 1040, 1000, true));
  KCD2S_CHECK(window.add(1100, 1100, 1120, 1000, false));
  KCD2S_CHECK_EQ(window.samples, 2u);
  KCD2S_CHECK_EQ(window.overlay_attempts, 1u);
  KCD2S_CHECK_EQ(window.first_sum_ms, 10.0);
  KCD2S_CHECK_EQ(window.second_sum_ms, 50.0);
  KCD2S_CHECK_EQ(window.total_sum_ms, 60.0);
  KCD2S_CHECK_EQ(window.first_max_ms, 10.0);
  KCD2S_CHECK_EQ(window.second_max_ms, 30.0);
  KCD2S_CHECK_EQ(window.total_max_ms, 40.0);
  KCD2S_CHECK_EQ(window.elapsed_ms(1000), 120.0);
}

KCD2S_TEST(pipeline_metrics, invalid_partial_and_overlapping_samples_do_not_dilute_means) {
  TimingWindow window;
  KCD2S_CHECK(window.add(100, 110, 130, 1000, true));
  ++window.aborted;
  ++window.nested;
  ++window.test_calls;
  KCD2S_CHECK(!window.add(0, 0, 0, 1000));
  KCD2S_CHECK(!window.add(200, 190, 210, 1000));
  KCD2S_CHECK(!window.add(200, 205, 204, 1000));
  KCD2S_CHECK(!window.add(200, 205, 210, 0));
  KCD2S_CHECK(!window.add(129, 140, 150, 1000));
  KCD2S_CHECK_EQ(window.samples, 1u);
  KCD2S_CHECK_EQ(window.invalid, 5u);
  KCD2S_CHECK_EQ(window.aborted, 1u);
  KCD2S_CHECK_EQ(window.total_sum_ms / window.samples, 30.0);
  KCD2S_CHECK_EQ(window.ended, 130);
  KCD2S_CHECK(window.add(130, 130, 130, 1000));
  KCD2S_CHECK_EQ(window.samples, 2u);
}

KCD2S_TEST(pipeline_metrics, thread_phase_windows_are_independent_and_reset_all_counters) {
  TimingWindow post;
  TimingWindow present;
  KCD2S_CHECK(post.add(100, 100, 400, 1000));
  KCD2S_CHECK(present.add(150, 200, 350, 1000, true));
  KCD2S_CHECK_EQ(post.total_sum_ms, 300.0);
  KCD2S_CHECK_EQ(present.total_sum_ms, 200.0);
  ++present.aborted;
  ++present.nested;
  ++present.test_calls;
  present = {};
  KCD2S_CHECK_EQ(present.samples, 0u);
  KCD2S_CHECK_EQ(present.aborted, 0u);
  KCD2S_CHECK_EQ(present.nested, 0u);
  KCD2S_CHECK_EQ(present.test_calls, 0u);
  KCD2S_CHECK_EQ(present.elapsed_ms(1000), 0.0);
  const auto end = std::numeric_limits<std::int64_t>::max();
  KCD2S_CHECK(present.add(end - 20, end - 10, end, 1000));
  KCD2S_CHECK_EQ(present.total_sum_ms, 20.0);
  KCD2S_CHECK_EQ(post.samples, 1u);
}

KCD2S_TEST(pipeline_metrics, post_children_partition_once_and_cpu_uses_100ns_units) {
  PostDetailSample sample;
  sample.begin = 100;
  sample.add_child(PostChild::render, 110, 310);
  sample.add_child(PostChild::render_end, 330, 380);
  PostDetailWindow window;
  window.add(sample, 400, 1000, {100000, 200000, true}, {110000, 290000, true});
  KCD2S_CHECK_EQ(window.partition_samples, 1u);
  KCD2S_CHECK_EQ(window.child_sum_ms[0], 200.0);
  KCD2S_CHECK_EQ(window.child_sum_ms[1], 50.0);
  KCD2S_CHECK_EQ(window.other_sum_ms, 50.0);
  KCD2S_CHECK_EQ(window.cpu_sum_ms, 10.0);
  KCD2S_CHECK_EQ(window.cpu_samples, 1u);
  PostDetailSample skipped;
  skipped.begin = 500;
  window.add(skipped, 510, 1000, {0, 0, true}, {0, 0, true});
  KCD2S_CHECK_EQ(window.partition_samples, 2u);
  KCD2S_CHECK_EQ(window.child_calls[0], 1u);
  KCD2S_CHECK_EQ(window.child_sum_ms[0] / window.partition_samples, 100.0);
  KCD2S_CHECK_EQ(window.other_sum_ms, 60.0);
  KCD2S_CHECK_EQ(window.cpu_sum_ms / window.cpu_samples, 5.0);
}

KCD2S_TEST(pipeline_metrics, bad_child_or_cpu_samples_have_independent_denominators) {
  PostDetailWindow window;
  PostDetailSample overlap;
  overlap.begin = 100;
  overlap.add_child(PostChild::render, 110, 180);
  overlap.add_child(PostChild::render_end, 170, 190);
  window.add(overlap, 200, 1000, {0, 0, true}, {10000, 0, true});
  KCD2S_CHECK_EQ(window.partition_samples, 0u);
  KCD2S_CHECK_EQ(window.partition_invalid, 1u);
  KCD2S_CHECK_EQ(window.cpu_samples, 1u);
  KCD2S_CHECK_EQ(window.cpu_sum_ms, 1.0);
  PostDetailSample complete;
  complete.begin = 300;
  complete.add_child(PostChild::render, 310, 330);
  complete.nested_children = 1;
  window.add(complete, 350, 1000, {}, {});
  KCD2S_CHECK_EQ(window.partition_samples, 1u);
  KCD2S_CHECK_EQ(window.child_sum_ms[0], 20.0);
  KCD2S_CHECK_EQ(window.nested_children, 1u);
  KCD2S_CHECK_EQ(window.cpu_invalid, 1u);
  complete.valid = false;  // A child unwound through SEH; outer caller recovered.
  window.add(complete, 350, 1000, {20, 30, true}, {19, 31, true});
  KCD2S_CHECK_EQ(window.partition_invalid, 2u);
  KCD2S_CHECK_EQ(window.cpu_invalid, 2u);
}

KCD2S_TEST(pipeline_metrics, post_child_bounds_and_large_cpu_counters_are_checked) {
  PostDetailWindow window;
  PostDetailSample beyond;
  beyond.begin = 100;
  beyond.add_child(PostChild::render, 120, 201);
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  window.add(beyond, 200, 1000, {maximum - 10000, maximum - 20000, true}, {maximum, maximum, true});
  KCD2S_CHECK_EQ(window.partition_invalid, 1u);
  KCD2S_CHECK_EQ(window.cpu_sum_ms, 3.0);
  PostDetailSample before;
  before.begin = 200;
  before.add_child(PostChild::render, 199, 220);
  KCD2S_CHECK(!before.valid);
  PostDetailSample reversed;
  reversed.begin = 200;
  reversed.add_child(PostChild::render, 220, 219);
  KCD2S_CHECK(!reversed.valid);
  window = {};
  KCD2S_CHECK_EQ(window.cpu_samples, 0u);
  KCD2S_CHECK_EQ(window.partition_invalid, 0u);
}

KCD2S_TEST(pipeline_metrics, animation_is_a_partition_but_nested_class_spans_are_inclusive) {
  PostDetailSample sample;
  sample.begin = 100;
  sample.animation.begin = 100;
  sample.animation.add(AnimationSpan::attachment_update, 130, 150);
  sample.animation.add(AnimationSpan::pose_finish, 120, 180);
  sample.animation.add(AnimationSpan::attachment_refresh, 190, 200);
  sample.animation.nested = 1;  // Recursive same-kind call was not timed again.
  sample.add_child(PostChild::animation_finish, 110, 210);
  sample.add_child(PostChild::render, 220, 250);
  sample.add_child(PostChild::render_end, 260, 270);
  PostDetailWindow window;
  window.add(sample, 300, 1000, {0, 0, true}, {10000, 0, true});
  KCD2S_CHECK_EQ(window.child_sum_ms[2], 100.0);
  KCD2S_CHECK_EQ(window.other_sum_ms, 60.0);
  KCD2S_CHECK_EQ(window.animation_samples, 1u);
  KCD2S_CHECK_EQ(window.animation_sum_ms[0], 60.0);
  KCD2S_CHECK_EQ(window.animation_sum_ms[1], 20.0);
  KCD2S_CHECK_EQ(window.animation_sum_ms[2], 10.0);
  KCD2S_CHECK_EQ(window.animation_nested, 1u);
  PostDetailSample no_manager;
  no_manager.begin = 310;
  window.add(no_manager, 330, 1000, {0, 0, true}, {0, 0, true});
  KCD2S_CHECK_EQ(window.animation_samples, 2u);
  KCD2S_CHECK_EQ(window.animation_calls[0], 1u);
  KCD2S_CHECK_EQ(window.animation_sum_ms[0] / window.animation_samples, 30.0);
}

KCD2S_TEST(pipeline_metrics, recovered_animation_fault_keeps_outer_partition_and_cpu) {
  PostDetailSample sample;
  sample.begin = 100;
  sample.animation.begin = 100;
  sample.animation.add(AnimationSpan::pose_finish, 110, 150);
  sample.animation.valid = false;  // Native manager recovered from child SEH.
  sample.add_child(PostChild::animation_finish, 105, 180);
  PostDetailWindow window;
  window.add(sample, 200, 1000, {0, 0, true}, {10000, 0, true});
  KCD2S_CHECK_EQ(window.partition_samples, 1u);
  KCD2S_CHECK_EQ(window.child_sum_ms[2], 75.0);
  KCD2S_CHECK_EQ(window.cpu_samples, 1u);
  KCD2S_CHECK_EQ(window.animation_samples, 0u);
  KCD2S_CHECK_EQ(window.animation_invalid, 1u);
  KCD2S_CHECK_EQ(window.animation_calls[0], 0u);
  sample.valid = false;  // Failed manager means no partial inclusive data either.
  window.add(sample, 200, 1000, {}, {});
  KCD2S_CHECK_EQ(window.animation_invalid, 1u);
  KCD2S_CHECK_EQ(window.partition_invalid, 1u);
}

KCD2S_TEST(pipeline_metrics, inclusive_spans_reject_same_kind_overlap_and_parent_escape) {
  PostDetailSample sample;
  sample.begin = 100;
  sample.animation.begin = 100;
  sample.animation.add(AnimationSpan::pose_finish, 110, 150);
  sample.animation.add(AnimationSpan::pose_finish, 140, 160);
  KCD2S_CHECK(!sample.animation.valid);
  sample.animation = {};
  sample.animation.begin = 100;
  sample.animation.add(AnimationSpan::attachment_update, 110, 201);
  PostDetailWindow window;
  window.add(sample, 200, 1000, {}, {});
  KCD2S_CHECK_EQ(window.animation_invalid, 1u);
  KCD2S_CHECK_EQ(window.animation_samples, 0u);
  sample.animation = {};
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  sample.begin = maximum - 100;
  sample.animation.begin = sample.begin;
  sample.animation.add(AnimationSpan::pose_finish, maximum - 90, maximum - 50);
  sample.animation.add(AnimationSpan::pose_finish, maximum - 50, maximum);
  window.add(sample, maximum, 1000, {}, {});
  KCD2S_CHECK_EQ(window.animation_samples, 1u);
  KCD2S_CHECK_EQ(window.animation_sum_ms[0], 90.0);
}
