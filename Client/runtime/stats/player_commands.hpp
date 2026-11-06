#pragma once

#include <cstddef>
#include <cstdint>

#include "../knockout/body_state.hpp"
#include "kcd2s/integration/player_engine_commands.hpp"

namespace kcds::runtime::stats {

// All callbacks run synchronously on the engine update thread. Observation must use the actual
// validated native body, never the knockout module's previous-tick cache. Each pop removes at
// most one command from the peer's bounded FIFO; the runner never stores a command or its tail.
struct PlayerCommandCallbacks {
  void* context = nullptr;
  kcd2s::integration::PlayerEngineCommandPoll (*pop)(
      void*, const kcd2s::integration::PlayerEngineScope&) = nullptr;
  bool (*is_current)(void*, const kcd2s::integration::PlayerEngineScope&) = nullptr;
  knockout::BodyState (*observe)(void*) = nullptr;
  void (*apply_stat)(void*, const kcd2s::integration::StatRequest&) = nullptr;
  bool (*revive)(void*) = nullptr;
  // Refresh native KO observation/reporting after every invoked mutation callback, including a
  // rejected revive: native setter work may precede remove-unconsciousness admission failure.
  void (*after_mutation)(void*) = nullptr;
};

enum class PlayerCommandRunStatus {
  Drained,
  BudgetExhausted,
  WaitingForNativeWake,
  ScopeChanged,
  Failed,
};

enum class PlayerCommandFailure {
  None,
  MissingCallback,
  PollRejected,
  MalformedCommand,
  ReviveRejected,
  BodyUnavailable,
};

struct PlayerCommandRunResult {
  PlayerCommandRunStatus status = PlayerCommandRunStatus::Drained;
  PlayerCommandFailure failure = PlayerCommandFailure::None;
  kcd2s::integration::PlayerEngineCommandStatus poll_status =
      kcd2s::integration::PlayerEngineCommandStatus::Empty;
  std::size_t popped = 0;
  std::size_t applied_stats = 0;
  std::size_t dropped_stats = 0;
  std::size_t revives = 0;
  std::uint64_t last_sequence = 0;
  std::uint64_t overflows = 0;
  std::uint64_t discarded_commands = 0;
};

class PlayerCommandRunner {
 public:
  static constexpr std::size_t kCommandsPerTick = 64;

  // Call only when binding a genuinely new world/owner/body scope, never just on KO, revive or
  // combat-epoch changes. Failed stops this scope: the caller must log and stop invoking it
  // until replacement. Read peer diagnostics before every tick, even while waiting for wake.
  void reset();
  PlayerCommandRunResult run_tick(const kcd2s::integration::PlayerEngineScope& scope,
                                  const PlayerCommandCallbacks& callbacks);
  bool waiting_for_native_wake() const;

 private:
  bool waiting_for_native_wake_ = false;
};

}  // namespace kcds::runtime::stats
