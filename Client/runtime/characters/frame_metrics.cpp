#include "frame_metrics.hpp"

#include "../core/log.hpp"

#include <algorithm>
#include <array>

namespace kcds::runtime::characters::frame_metrics {
namespace {

bool flag(const wchar_t* name) {
  wchar_t value[4]{};
  return GetEnvironmentVariableW(name, value, 4) == 1 && value[0] == L'1';
}

struct Window {
  std::int64_t started = 0;
  std::uint64_t samples = 0;
  double native_sum_ms = 0;
  double adapter_sum_ms = 0;
  double native_max_ms = 0;
  double adapter_max_ms = 0;
  std::array<double, static_cast<std::size_t>(Phase::Count)> phases_ms{};
  double outside_sum_ms = 0;
  double outside_max_ms = 0;
  std::uint64_t outside_samples = 0;
  std::uint64_t frame_entered = 0;
  std::uint64_t adapter_failed = 0;
};

thread_local Window window;
thread_local std::int64_t previous_end = 0;
thread_local std::array<double, static_cast<std::size_t>(Phase::Count)> pending_phases{};
thread_local bool frame_entered = false;

double ticks_per_ms() {
  static const double value = [] {
    LARGE_INTEGER frequency{};
    return QueryPerformanceFrequency(&frequency) ? double(frequency.QuadPart) / 1000.0 : 0.0;
  }();
  return value;
}

}  // namespace

std::int64_t stamp() {
  static const bool enabled = flag(L"KCDS_TEST_HOOKS") && flag(L"KCDS_TEST_CHARACTER_METRICS");
  if (!enabled) {
    return 0;
  }
  LARGE_INTEGER value{};
  return QueryPerformanceCounter(&value) ? value.QuadPart : 0;
}

Phases::Phases() : previous_(stamp()) {
  if (previous_) {
    pending_phases = {};
    frame_entered = true;
  }
}

void Phases::mark(Phase phase) {
  if (!previous_) {
    return;
  }
  const auto end = stamp();
  const double frequency = ticks_per_ms();
  const auto index = static_cast<std::size_t>(phase);
  if (end >= previous_ && frequency && index < pending_phases.size()) {
    pending_phases[index] += double(end - previous_) / frequency;
  }
  previous_ = end;
}

void finish(std::int64_t begin, std::int64_t native_end, bool adapter_failed) {
  if (!begin || !native_end) {
    pending_phases = {};
    frame_entered = false;
    previous_end = 0;
    return;
  }
  const auto end = stamp();
  const double frequency = ticks_per_ms();
  if (!frequency || native_end < begin || end < native_end) {
    pending_phases = {};
    frame_entered = false;
    previous_end = 0;
    return;
  }
  // No shared diagnostic state if the engine ever dispatches updates on another thread.
  if (!window.started) {
    window.started = begin;
  }
  const double native_ms = double(native_end - begin) / frequency;
  const double adapter_ms = double(end - native_end) / frequency;
  // Finalize outside frame(): /EHsc need not run its C++ destructors on native SEH.
  // An unfinished region belongs to Other, never to the next update's phase sample.
  if (frame_entered) {
    double accounted = 0;
    for (std::size_t i = 0; i < pending_phases.size(); ++i) {
      accounted += pending_phases[i];
      window.phases_ms[i] += pending_phases[i];
    }
    window.phases_ms[static_cast<std::size_t>(Phase::Other)] +=
        std::max(0.0, adapter_ms - accounted);
    ++window.frame_entered;
  }
  pending_phases = {};
  frame_entered = false;
  window.adapter_failed += adapter_failed ? 1 : 0;
  if (previous_end && begin >= previous_end) {
    const double outside_ms = double(begin - previous_end) / frequency;
    window.outside_sum_ms += outside_ms;
    window.outside_max_ms = std::max(window.outside_max_ms, outside_ms);
    ++window.outside_samples;
  }
  previous_end = end;
  window.native_sum_ms += native_ms;
  window.adapter_sum_ms += adapter_ms;
  window.native_max_ms = std::max(window.native_max_ms, native_ms);
  window.adapter_max_ms = std::max(window.adapter_max_ms, adapter_ms);
  ++window.samples;
  const double elapsed_ms = double(end - window.started) / frequency;
  if (elapsed_ms < 5000) {
    return;
  }
  kcds::game_client::log(
      "CHARACTER FRAME interval_ms=%.2f samples=%llu thread=%lu native_update_mean_ms=%.3f "
      "native_update_max_ms=%.3f adapter_mean_ms=%.3f adapter_max_ms=%.3f "
      "measurement=wall_time_including_waits",
      elapsed_ms, static_cast<unsigned long long>(window.samples), GetCurrentThreadId(),
      window.native_sum_ms / window.samples, window.native_max_ms,
      window.adapter_sum_ms / window.samples, window.adapter_max_ms);
  const auto mean = [](Phase phase) {
    return window.frame_entered
               ? window.phases_ms[static_cast<std::size_t>(phase)] / window.frame_entered
               : 0.0;
  };
  kcds::game_client::log(
      "CHARACTER PHASES updates=%llu frame_entered=%llu frame_skipped=%llu adapter_failed=%llu "
      "setup_ms=%.3f mirror_ms=%.3f local_ms=%.3f "
      "local_state_ms=%.3f retirement_ms=%.3f horses_ms=%.3f npcs_ms=%.3f "
      "features_ms=%.3f players_ms=%.3f combat_ms=%.3f other_ms=%.3f "
      "outside_update_samples=%llu outside_update_mean_ms=%.3f outside_update_max_ms=%.3f "
      "measurement=wall_time_including_waits phase_means=per_entered_frame",
      static_cast<unsigned long long>(window.samples),
      static_cast<unsigned long long>(window.frame_entered),
      static_cast<unsigned long long>(window.samples - window.frame_entered),
      static_cast<unsigned long long>(window.adapter_failed), mean(Phase::Setup),
      mean(Phase::Mirror), mean(Phase::Local), mean(Phase::LocalState), mean(Phase::Retirement),
      mean(Phase::Horses), mean(Phase::Npcs), mean(Phase::Features), mean(Phase::Players),
      mean(Phase::Combat), mean(Phase::Other),
      static_cast<unsigned long long>(window.outside_samples),
      window.outside_samples ? window.outside_sum_ms / window.outside_samples : 0.0,
      window.outside_max_ms);
  window = {};
}

}  // namespace kcds::runtime::characters::frame_metrics
