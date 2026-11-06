#include "pipeline_metrics.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"
#include "pipeline_metrics_sample.hpp"

#include <mutex>
#include <type_traits>

namespace kcds::runtime::characters::pipeline_metrics {
namespace {

// IDA2, WHGame 1.5.5: CCryAction slot11, class method180669C88. This wildcard
// signature has one whole-image match in SHA256
// bdf8f9e4a11257a72b64c84700e284c29e4c4ccaf5b8d4bfa7d0b2a7294479f7.
// The adjacent slot10 is the existing update hook; this phase includes native
// world rendering, callbacks and frame completion. Addresses are evidence only.
constexpr const char* kPostPattern =
    "41 F6 C0 20 0F 85 ?? ?? ?? ?? 48 8B C4 48 89 58 08 48 89 70 10 44 89 40 18 55";
// CSystem slots9/11: Render18043E464 and RenderEnd1809A7AE0. Each full
// entry signature has one executable whole-image match in the same pinned DLL.
constexpr const char* kRenderPattern =
    "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 56 41 57 48 8D 68 A1 "
    "48 81 EC C0 00 00 00 80 B9 C2 05 00 00 00 4C 8B FA 48 8B F1";
constexpr const char* kRenderEndPattern =
    "48 89 5C 24 08 57 48 83 EC 20 80 B9 C2 05 00 00 00 40 8A FA 48 8B D9";
// CharacterManager slot34, CSkeletonPose completion,
// CAttachmentManager pose update and render-data refresh.
// All four signatures match once in executable .text in the pinned whole image.
// The shorter pose/update prologues each matched twice; keep this full context.
constexpr const char* kAnimationFinishPattern =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 "
    "48 8D AC 24 80 FC FF FF 48 81 EC 80 04 00 00";
constexpr const char* kPoseFinishPattern =
    "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 80 FE FF FF "
    "48 81 EC 80 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 70 01 00 00 "
    "48 8B B1 38 02 00 00";
constexpr const char* kAttachmentUpdatePattern =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8D 59 18 "
    "48 8B EA 48 8D B1 B8 00 00 00 48 8B CB E8 ?? ?? ?? ?? 0F B7 7E 28";
constexpr const char* kAttachmentRefreshPattern =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8D B9 B8 "
    "00 00 00 48 8B F1 80 7F 44 00";
constexpr double kReportMilliseconds = 5000.0;
using PostUpdate = void(__fastcall*)(void*, std::uintptr_t, int);
// Render's interface callers discard RAX. Preserve the raw native result anyway;
// its skipped path leaves an incidental stack address, not a semantic pointer.
using SystemRender = std::uintptr_t(__fastcall*)(void*, int*);
using SystemRenderEnd = void(__fastcall*)(void*, char);
using AnimationFinish = std::uintptr_t(__fastcall*)(void*);
// Preserve all four integer-register arguments: the native pose routine
// overwrites R8 but forwards incoming R9 to a helper. Never narrow that value.
using PoseFinish = std::uintptr_t(__fastcall*)(void*, void*, std::uintptr_t, std::uintptr_t);
using AttachmentUpdate = std::uintptr_t(__fastcall*)(void*, void*);
using AttachmentRefresh = std::uintptr_t(__fastcall*)(void*);
PostUpdate original_post = nullptr;
SystemRender original_render = nullptr;
SystemRenderEnd original_render_end = nullptr;
AnimationFinish original_animation_finish = nullptr;
PoseFinish original_pose_finish = nullptr;
AttachmentUpdate original_attachment_update = nullptr;
AttachmentRefresh original_attachment_refresh = nullptr;
thread_local bool inside_post = false;
thread_local bool inside_present = false;
thread_local bool inside_post_child = false;
thread_local PostDetailSample* active_post = nullptr;
thread_local AnimationDetailSample* active_animation = nullptr;
thread_local unsigned animation_span_mask = 0;
thread_local TimingWindow post_window;
thread_local PostDetailWindow post_details;
thread_local TimingWindow present_window;
std::mutex install_mutex;
bool attempted = false;
bool installed = false;

static_assert(std::is_trivially_destructible_v<PresentSpan>);
static_assert(std::is_trivially_destructible_v<PostDetailSample>);

bool flag(const wchar_t* name) {
  wchar_t value[4]{};
  return GetEnvironmentVariableW(name, value, 4) == 1 && value[0] == L'1';
}

bool enabled() {
  static const bool value = flag(L"KCDS_TEST_HOOKS") && flag(L"KCDS_TEST_CHARACTER_METRICS");
  return value;
}

std::int64_t stamp() {
  LARGE_INTEGER value{};
  return QueryPerformanceCounter(&value) ? value.QuadPart : 0;
}

std::int64_t frequency() {
  static const std::int64_t value = [] {
    LARGE_INTEGER result{};
    return QueryPerformanceFrequency(&result) ? result.QuadPart : 0;
  }();
  return value;
}

ThreadCpuStamp thread_cpu() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
    return {};
  }
  return {(std::uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime,
          (std::uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime, true};
}

void publish_post(const PostDetailSample& sample, std::int64_t end, int flags,
                  const ThreadCpuStamp& cpu_begin, const ThreadCpuStamp& cpu_end) {
  const auto rate = frequency();
  if (!post_window.add(sample.begin, sample.begin, end, rate)) {
    return;
  }
  post_details.add(sample, end, rate, cpu_begin, cpu_end);
  post_window.flag_skip_calls += (flags & 0x20) != 0 ? 1 : 0;
  if (post_window.elapsed_ms(rate) < kReportMilliseconds) {
    return;
  }
  const auto report = post_window;
  const auto detail = post_details;
  post_window = {};
  post_details = {};
  const double partitions = detail.partition_samples ? double(detail.partition_samples) : 1.0;
  const double cpu_samples = detail.cpu_samples ? double(detail.cpu_samples) : 1.0;
  const double animation_samples =
      detail.animation_samples ? double(detail.animation_samples) : 1.0;
  kcds::game_client::log(
      "CHARACTER POST interval_ms=%.2f samples=%llu thread=%lu post_mean_ms=%.3f "
      "post_max_ms=%.3f aborted=%llu invalid=%llu nested=%llu flag_skip_calls=%llu "
      "partition_samples=%llu partition_invalid=%llu child_nested=%llu render_calls=%llu "
      "render_mean_ms=%.3f render_max_ms=%.3f render_end_calls=%llu render_end_mean_ms=%.3f "
      "render_end_max_ms=%.3f animation_finish_calls=%llu animation_finish_mean_ms=%.3f "
      "animation_finish_max_ms=%.3f other_mean_ms=%.3f other_max_ms=%.3f "
      "animation_samples=%llu animation_invalid=%llu animation_nested=%llu "
      "pose_finish_calls=%llu pose_finish_mean_ms=%.3f pose_finish_max_ms=%.3f "
      "attachment_update_calls=%llu attachment_update_mean_ms=%.3f attachment_update_max_ms=%.3f "
      "attachment_refresh_calls=%llu attachment_refresh_mean_ms=%.3f "
      "attachment_refresh_max_ms=%.3f "
      "cpu_samples=%llu cpu_invalid=%llu thread_cpu_mean_ms=%.3f thread_cpu_max_ms=%.3f "
      "measurement=wall_time_including_waits cpu=post_thread_kernel_plus_user "
      "child_means=per_valid_post animation_spans=inclusive_do_not_sum "
      "animation_threads=post_thread_only scope=outer_calls overlap=independent_windows",
      report.elapsed_ms(rate), static_cast<unsigned long long>(report.samples),
      GetCurrentThreadId(), report.total_sum_ms / report.samples, report.total_max_ms,
      static_cast<unsigned long long>(report.aborted),
      static_cast<unsigned long long>(report.invalid),
      static_cast<unsigned long long>(report.nested),
      static_cast<unsigned long long>(report.flag_skip_calls),
      static_cast<unsigned long long>(detail.partition_samples),
      static_cast<unsigned long long>(detail.partition_invalid),
      static_cast<unsigned long long>(detail.nested_children),
      static_cast<unsigned long long>(detail.child_calls[0]), detail.child_sum_ms[0] / partitions,
      detail.child_max_ms[0], static_cast<unsigned long long>(detail.child_calls[1]),
      detail.child_sum_ms[1] / partitions, detail.child_max_ms[1],
      static_cast<unsigned long long>(detail.child_calls[2]), detail.child_sum_ms[2] / partitions,
      detail.child_max_ms[2], detail.other_sum_ms / partitions, detail.other_max_ms,
      static_cast<unsigned long long>(detail.animation_samples),
      static_cast<unsigned long long>(detail.animation_invalid),
      static_cast<unsigned long long>(detail.animation_nested),
      static_cast<unsigned long long>(detail.animation_calls[0]),
      detail.animation_sum_ms[0] / animation_samples, detail.animation_max_ms[0],
      static_cast<unsigned long long>(detail.animation_calls[1]),
      detail.animation_sum_ms[1] / animation_samples, detail.animation_max_ms[1],
      static_cast<unsigned long long>(detail.animation_calls[2]),
      detail.animation_sum_ms[2] / animation_samples, detail.animation_max_ms[2],
      static_cast<unsigned long long>(detail.cpu_samples),
      static_cast<unsigned long long>(detail.cpu_invalid), detail.cpu_sum_ms / cpu_samples,
      detail.cpu_max_ms);
}

void __fastcall post_update(void* self, std::uintptr_t argument, int flags) {
  const bool previous = inside_post;
  auto* const previous_post = active_post;
  auto* const previous_animation = active_animation;
  PostDetailSample sample;
  sample.begin = previous ? 0 : stamp();
  const auto cpu_begin = previous ? ThreadCpuStamp{} : thread_cpu();
  ThreadCpuStamp cpu_end;
  std::int64_t end = 0;
  bool completed = false;
  if (previous) {
    ++post_window.nested;
  }
  inside_post = true;
  // A reentrant post remains included in outer wall/CPU time, but its children
  // are not separately attributed to the outer post's direct child partition.
  active_post = previous ? nullptr : &sample;
  active_animation = nullptr;
  __try {
    original_post(self, argument, flags);
    if (!previous) {
      cpu_end = thread_cpu();
      end = stamp();
    }
    completed = true;
  } __finally {
    inside_post = previous;
    active_post = previous_post;
    active_animation = previous_animation;
    if (!previous && !completed) {
      ++post_window.aborted;
    }
  }
  if (!previous) {
    publish_post(sample, end, flags, cpu_begin, cpu_end);
  }
}

std::uintptr_t __fastcall system_render(void* self, int* pass) {
  auto* const sample = active_post;
  const bool previous = inside_post_child;
  const bool tracked = sample && !previous;
  const auto begin = tracked ? stamp() : 0;
  std::int64_t end = 0;
  std::uintptr_t result = 0;
  bool completed = false;
  if (sample && previous) {
    ++sample->nested_children;
  }
  inside_post_child = true;
  __try {
    result = original_render(self, pass);
    if (tracked) {
      end = stamp();
    }
    completed = true;
  } __finally {
    inside_post_child = previous;
    if (tracked && !completed) {
      sample->valid = false;
    }
  }
  if (tracked) {
    sample->add_child(PostChild::render, begin, end);
  }
  return result;
}

void __fastcall system_render_end(void* self, char render_stats) {
  auto* const sample = active_post;
  const bool previous = inside_post_child;
  const bool tracked = sample && !previous;
  const auto begin = tracked ? stamp() : 0;
  std::int64_t end = 0;
  bool completed = false;
  if (sample && previous) {
    ++sample->nested_children;
  }
  inside_post_child = true;
  __try {
    original_render_end(self, render_stats);
    if (tracked) {
      end = stamp();
    }
    completed = true;
  } __finally {
    inside_post_child = previous;
    if (tracked && !completed) {
      sample->valid = false;
    }
  }
  if (tracked) {
    sample->add_child(PostChild::render_end, begin, end);
  }
}

std::uintptr_t __fastcall animation_finish(void* self) {
  auto* const sample = active_post;
  auto* const previous_animation = active_animation;
  const auto previous_mask = animation_span_mask;
  const bool previous = inside_post_child;
  const bool tracked = sample && !previous;
  const auto begin = tracked ? stamp() : 0;
  std::int64_t end = 0;
  std::uintptr_t result = 0;
  bool completed = false;
  if (sample && previous) {
    ++sample->nested_children;
  }
  inside_post_child = true;
  active_animation = tracked ? &sample->animation : nullptr;
  animation_span_mask = 0;
  if (tracked) {
    sample->animation.begin = sample->begin;
  }
  __try {
    result = original_animation_finish(self);
    if (tracked) {
      end = stamp();
    }
    completed = true;
  } __finally {
    inside_post_child = previous;
    active_animation = previous_animation;
    animation_span_mask = previous_mask;
    if (tracked && !completed) {
      sample->valid = false;
      sample->animation.valid = false;
    }
  }
  if (tracked) {
    sample->add_child(PostChild::animation_finish, begin, end);
  }
  return result;
}

struct AnimationCall {
  AnimationDetailSample* sample;
  AnimationSpan kind;
  unsigned previous_mask;
  bool tracked;
  std::int64_t begin;
};
static_assert(std::is_trivially_destructible_v<AnimationCall>);

AnimationCall begin_animation_call(AnimationSpan kind) {
  const auto bit = 1u << static_cast<unsigned>(kind);
  const bool tracked = (animation_span_mask & bit) == 0;
  AnimationCall call{active_animation, kind, animation_span_mask, tracked, tracked ? stamp() : 0};
  if (!tracked) {
    ++call.sample->nested;
  }
  animation_span_mask |= bit;
  return call;
}

void leave_animation_call(const AnimationCall& call, std::int64_t end, bool completed) {
  animation_span_mask = call.previous_mask;
  if (!completed) {
    call.sample->valid = false;
  } else if (call.tracked) {
    call.sample->add(call.kind, call.begin, end);
  }
}

std::uintptr_t __fastcall pose_finish(void* self, void* pose_data, std::uintptr_t argument3,
                                      std::uintptr_t argument4) {
  // Worker-thread completions and calls outside this manager phase are excluded.
  // No cross-thread pointers, counters, waits or locks enter this diagnostic.
  if (!active_animation) {
    return original_pose_finish(self, pose_data, argument3, argument4);
  }
  const auto call = begin_animation_call(AnimationSpan::pose_finish);
  std::uintptr_t result = 0;
  std::int64_t end = 0;
  bool completed = false;
  __try {
    result = original_pose_finish(self, pose_data, argument3, argument4);
    end = call.tracked ? stamp() : 0;
    completed = true;
  } __finally {
    leave_animation_call(call, end, completed);
  }
  return result;
}

std::uintptr_t __fastcall attachment_update(void* self, void* pose_data) {
  if (!active_animation) {
    return original_attachment_update(self, pose_data);
  }
  const auto call = begin_animation_call(AnimationSpan::attachment_update);
  std::uintptr_t result = 0;
  std::int64_t end = 0;
  bool completed = false;
  __try {
    result = original_attachment_update(self, pose_data);
    end = call.tracked ? stamp() : 0;
    completed = true;
  } __finally {
    leave_animation_call(call, end, completed);
  }
  return result;
}

std::uintptr_t __fastcall attachment_refresh(void* self) {
  if (!active_animation) {
    return original_attachment_refresh(self);
  }
  const auto call = begin_animation_call(AnimationSpan::attachment_refresh);
  std::uintptr_t result = 0;
  std::int64_t end = 0;
  bool completed = false;
  __try {
    result = original_attachment_refresh(self);
    end = call.tracked ? stamp() : 0;
    completed = true;
  } __finally {
    leave_animation_call(call, end, completed);
  }
  return result;
}

bool executable(void* target) {
  MEMORY_BASIC_INFORMATION region{};
  if (!VirtualQuery(target, &region, sizeof(region)) || region.State != MEM_COMMIT ||
      (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return false;
  }
  const auto protection = region.Protect & 0xff;
  return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
         protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

}  // namespace

bool install() {
  if (!enabled()) {
    return true;
  }
  std::lock_guard<std::mutex> lock(install_mutex);
  if (attempted) {
    return installed;
  }
  attempted = true;
  struct Hook {
    const char* name;
    const char* pattern;
    void* replacement;
    void** original;
    void* target = nullptr;
  };
  Hook hooks[]{
      {"CharacterPostMetrics", kPostPattern, reinterpret_cast<void*>(post_update),
       reinterpret_cast<void**>(&original_post)},
      {"CharacterRenderMetrics", kRenderPattern, reinterpret_cast<void*>(system_render),
       reinterpret_cast<void**>(&original_render)},
      {"CharacterRenderEndMetrics", kRenderEndPattern, reinterpret_cast<void*>(system_render_end),
       reinterpret_cast<void**>(&original_render_end)},
      {"CharacterAnimationFinishMetrics", kAnimationFinishPattern,
       reinterpret_cast<void*>(animation_finish),
       reinterpret_cast<void**>(&original_animation_finish)},
      {"CharacterPoseFinishMetrics", kPoseFinishPattern, reinterpret_cast<void*>(pose_finish),
       reinterpret_cast<void**>(&original_pose_finish)},
      {"CharacterAttachmentUpdateMetrics", kAttachmentUpdatePattern,
       reinterpret_cast<void*>(attachment_update),
       reinterpret_cast<void**>(&original_attachment_update)},
      {"CharacterAttachmentRefreshMetrics", kAttachmentRefreshPattern,
       reinterpret_cast<void*>(attachment_refresh),
       reinterpret_cast<void**>(&original_attachment_refresh)}};
  if (!frequency()) {
    kcds::game_client::log("CHARACTER POST diagnostics unavailable: timer rejected");
    return false;
  }
  for (auto& hook : hooks) {
    hook.target = kcds::game_client::resolve_unique(hook.name, hook.pattern);
    if (!hook.target || !executable(hook.target)) {
      kcds::game_client::log("CHARACTER POST diagnostics unavailable: signature rejected");
      return false;
    }
  }
  std::size_t created = 0;
  bool hooks_ok = true;
  for (auto& hook : hooks) {
    if (MH_CreateHook(hook.target, hook.replacement, hook.original) != MH_OK) {
      hooks_ok = false;
      break;
    }
    ++created;
  }
  if (hooks_ok) {
    for (const auto& hook : hooks) {
      if (MH_EnableHook(hook.target) != MH_OK) {
        hooks_ok = false;
        break;
      }
    }
  }
  if (!hooks_ok) {
    while (created) {
      auto& hook = hooks[--created];
      MH_DisableHook(hook.target);
      MH_RemoveHook(hook.target);
      *hook.original = nullptr;
    }
    kcds::game_client::log("CHARACTER POST diagnostics unavailable: hook installation failed");
    return false;
  }
  installed = true;
  kcds::game_client::log(
      "CHARACTER PIPELINE diagnostics installed post=class_method "
      "children=system_render_render_end_animation_finish present=existing_hook "
      "animation_spans=pose_attachment_update_attachment_refresh_inclusive "
      "animation_threads=post_thread_only "
      "measurement=wall_time_including_waits cpu=post_thread_kernel_plus_user "
      "overlap=independent_windows");
  return true;
}

PresentSpan begin_present(bool test_call) {
  PresentSpan span;
  if (!enabled()) {
    return span;
  }
  span.tracked = true;
  span.previous = inside_present;
  span.outer = !span.previous && !test_call;
  if (span.previous) {
    ++present_window.nested;
  }
  if (test_call) {
    ++present_window.test_calls;
  }
  inside_present = true;
  if (span.outer) {
    span.begin = stamp();
  }
  return span;
}

void overlay_finished(PresentSpan& span, bool attempted) {
  if (span.outer) {
    span.overlay_end = stamp();
    span.overlay_attempted = attempted;
  }
}

void native_present_finished(PresentSpan& span) {
  if (span.outer) {
    span.end = stamp();
  }
}

void leave_present(const PresentSpan& span, bool completed) {
  if (span.tracked) {
    inside_present = span.previous;
    if (span.outer && !completed) {
      ++present_window.aborted;
    }
  }
}

void publish_present(const PresentSpan& span) {
  if (!span.outer) {
    return;
  }
  const auto rate = frequency();
  if (!present_window.add(span.begin, span.overlay_end, span.end, rate, span.overlay_attempted) ||
      present_window.elapsed_ms(rate) < kReportMilliseconds) {
    return;
  }
  const auto report = present_window;
  present_window = {};
  kcds::game_client::log(
      "CHARACTER PRESENT interval_ms=%.2f samples=%llu thread=%lu overlay_attempts=%llu "
      "overlay_mean_ms=%.3f overlay_max_ms=%.3f native_present_mean_ms=%.3f "
      "native_present_max_ms=%.3f total_mean_ms=%.3f total_max_ms=%.3f "
      "aborted=%llu invalid=%llu nested=%llu test_calls=%llu "
      "measurement=wall_time_including_waits scope=outer_non_test_calls "
      "overlap=independent_windows",
      report.elapsed_ms(rate), static_cast<unsigned long long>(report.samples),
      GetCurrentThreadId(), static_cast<unsigned long long>(report.overlay_attempts),
      report.first_sum_ms / report.samples, report.first_max_ms,
      report.second_sum_ms / report.samples, report.second_max_ms,
      report.total_sum_ms / report.samples, report.total_max_ms,
      static_cast<unsigned long long>(report.aborted),
      static_cast<unsigned long long>(report.invalid),
      static_cast<unsigned long long>(report.nested),
      static_cast<unsigned long long>(report.test_calls));
}

}  // namespace kcds::runtime::characters::pipeline_metrics
