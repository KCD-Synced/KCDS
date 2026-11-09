#include "player_commands.hpp"

#include <functional>
#include <optional>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {

namespace integration = kcd2s::integration;
namespace stats = kcds::runtime::stats;
using kcds::runtime::knockout::BodyState;
using stats::PlayerCommandFailure;
using stats::PlayerCommandRunStatus;

enum class EventKind {
  Stat,
  Revive,
  KnockOut,
  Refresh,
};

struct Event {
  EventKind kind;
  float value = 0;
};

struct Engine {
  integration::PlayerEngineScope scope{1, 7, 1};
  integration::PlayerEngineCommandQueue queue;
  stats::PlayerCommandRunner runner;
  BodyState actual = BodyState::Awake;
  bool cached_knocked_out = false;
  bool current = true;
  bool admit_revive = true;
  std::size_t pop_calls = 0;
  std::size_t observe_calls = 0;
  std::vector<integration::StatRequest> applied;
  std::vector<Event> events;
  std::optional<integration::PlayerEngineCommandPoll> poll_override;
  std::function<void(Engine&)> on_pop;
  std::function<void(Engine&)> on_observe;
  std::function<void(Engine&, const integration::StatRequest&)> on_stat;
  std::function<void(Engine&)> on_revive;
  std::function<void(Engine&)> on_refresh;

  Engine() {
    queue.reset_world(scope.revision);
    queue.bind(scope);
  }

  bool stat(float value, std::uint8_t id = 0) {
    return queue.push_stat(scope.revision, {id, value});
  }

  bool revive() {
    return queue.push_revive(scope.revision, scope.owner);
  }

  bool knock_out(float health) {
    return queue.push_knock_out(scope.revision, health);
  }

  stats::PlayerCommandCallbacks callbacks() {
    stats::PlayerCommandCallbacks result;
    result.context = this;
    result.pop = [](void* context, const integration::PlayerEngineScope& scope) {
      auto& engine = *static_cast<Engine*>(context);
      ++engine.pop_calls;
      auto result = engine.poll_override ? *engine.poll_override : engine.queue.take(scope);
      if (engine.on_pop) {
        engine.on_pop(engine);
      }
      return result;
    };
    result.is_current = [](void* context, const integration::PlayerEngineScope& scope) {
      auto& engine = *static_cast<Engine*>(context);
      return engine.current && engine.scope == scope;
    };
    result.observe = [](void* context) {
      auto& engine = *static_cast<Engine*>(context);
      ++engine.observe_calls;
      if (engine.on_observe) {
        engine.on_observe(engine);
      }
      return engine.actual;
    };
    result.apply_stat = [](void* context, const integration::StatRequest& request) {
      auto& engine = *static_cast<Engine*>(context);
      engine.applied.push_back(request);
      engine.events.push_back({EventKind::Stat, request.value});
      if (engine.on_stat) {
        engine.on_stat(engine, request);
      }
    };
    result.revive = [](void* context) {
      auto& engine = *static_cast<Engine*>(context);
      engine.events.push_back({EventKind::Revive});
      if (engine.on_revive) {
        engine.on_revive(engine);
      }
      return engine.admit_revive;
    };
    result.knock_out = [](void* context, float health) {
      auto& engine = *static_cast<Engine*>(context);
      engine.events.push_back({EventKind::KnockOut, health});
      engine.actual = BodyState::KnockedOut;
      return true;
    };
    result.after_mutation = [](void* context) {
      auto& engine = *static_cast<Engine*>(context);
      engine.events.push_back({EventKind::Refresh});
      if (engine.on_refresh) {
        engine.on_refresh(engine);
      }
    };
    return result;
  }

  stats::PlayerCommandRunResult tick() {
    return runner.run_tick(scope, callbacks());
  }
};

void native_wake(Engine& engine) {
  engine.actual = BodyState::Awake;
}

void lose_scope(Engine& engine) {
  engine.current = false;
}

void replenish_stat(Engine& engine, const integration::StatRequest&) {
  engine.stat(50);
}

}  // namespace

KCD2S_TEST(runtime_player_commands, native_zero_health_ko_drops_later_stats) {
  Engine engine;
  engine.on_stat = [](Engine& state, const integration::StatRequest& request) {
    if (request.stat == 0 && request.value == 0) {
      state.actual = BodyState::KnockedOut;
    }
  };
  KCD2S_CHECK(engine.stat(0));
  KCD2S_CHECK(engine.stat(50));
  KCD2S_CHECK(engine.stat(20, 1));

  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::Drained);
  KCD2S_CHECK_EQ(result.popped, std::size_t(3));
  KCD2S_CHECK_EQ(result.applied_stats, std::size_t(1));
  KCD2S_CHECK_EQ(result.dropped_stats, std::size_t(2));
  KCD2S_CHECK_EQ(engine.applied.size(), std::size_t(1));
  KCD2S_CHECK(engine.applied.front().value == 0);
  KCD2S_CHECK_EQ(engine.events.size(), std::size_t(2));
  KCD2S_CHECK(engine.events.back().kind == EventKind::Refresh);
}

KCD2S_TEST(runtime_player_commands, native_zero_health_veto_allows_later_stats) {
  Engine engine;
  // The native damage ruling vetoes lethal damage, leaving the actual body awake.
  KCD2S_CHECK(engine.stat(0));
  KCD2S_CHECK(engine.stat(50));
  KCD2S_CHECK(engine.stat(20, 1));

  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::Drained);
  KCD2S_CHECK_EQ(result.applied_stats, std::size_t(3));
  KCD2S_CHECK_EQ(result.dropped_stats, std::size_t(0));
  KCD2S_CHECK_EQ(engine.observe_calls, std::size_t(3));
  KCD2S_CHECK(engine.applied[1].value == 50);
  KCD2S_CHECK(engine.applied[2].stat == 1);
  KCD2S_CHECK_EQ(engine.events.size(), std::size_t(6));
}

KCD2S_TEST(runtime_player_commands, zero_then_revive_then_stat_keeps_native_event_order) {
  Engine engine;
  engine.on_stat = [](Engine& state, const integration::StatRequest& request) {
    if (request.stat == 0 && request.value == 0) {
      state.actual = BodyState::KnockedOut;
    }
  };
  engine.on_revive = native_wake;
  KCD2S_CHECK(engine.stat(0));
  KCD2S_CHECK(engine.revive());
  KCD2S_CHECK(engine.stat(65));

  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::Drained);
  KCD2S_CHECK_EQ(result.applied_stats, std::size_t(2));
  KCD2S_CHECK_EQ(result.revives, std::size_t(1));
  KCD2S_CHECK_EQ(engine.events.size(), std::size_t(6));
  KCD2S_CHECK(engine.events[0].kind == EventKind::Stat);
  KCD2S_CHECK(engine.events[0].value == 0);
  KCD2S_CHECK(engine.events[1].kind == EventKind::Refresh);
  KCD2S_CHECK(engine.events[2].kind == EventKind::Revive);
  KCD2S_CHECK(engine.events[3].kind == EventKind::Refresh);
  KCD2S_CHECK(engine.events[4].kind == EventKind::Stat);
  KCD2S_CHECK(engine.events[4].value == 65);
  KCD2S_CHECK(engine.events[5].kind == EventKind::Refresh);
}

KCD2S_TEST(runtime_player_commands, receipt_order_and_actual_wake_override_stale_ko_cache) {
  Engine engine;
  engine.actual = BodyState::KnockedOut;
  engine.cached_knocked_out = true;
  engine.on_revive = native_wake;
  KCD2S_CHECK(engine.stat(40));
  KCD2S_CHECK(engine.revive());
  KCD2S_CHECK(engine.stat(65));

  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::Drained);
  KCD2S_CHECK_EQ(result.dropped_stats, std::size_t(1));
  KCD2S_CHECK_EQ(result.revives, std::size_t(1));
  KCD2S_CHECK_EQ(result.applied_stats, std::size_t(1));
  KCD2S_CHECK_EQ(engine.events.size(), std::size_t(4));
  KCD2S_CHECK(engine.events[0].kind == EventKind::Revive);
  KCD2S_CHECK(engine.events[1].kind == EventKind::Refresh);
  KCD2S_CHECK(engine.events[2].kind == EventKind::Stat);
  KCD2S_CHECK(engine.events[2].value == 65);
  KCD2S_CHECK(engine.events[3].kind == EventKind::Refresh);
  KCD2S_CHECK(engine.cached_knocked_out);
  KCD2S_CHECK(!engine.runner.waiting_for_native_wake());
}

KCD2S_TEST(runtime_player_commands, deferred_wake_preserves_tail_without_repeating_revive) {
  Engine engine;
  engine.actual = BodyState::KnockedOut;
  KCD2S_CHECK(engine.revive());
  KCD2S_CHECK(engine.stat(65));
  KCD2S_CHECK(engine.revive());
  KCD2S_CHECK(engine.stat(20, 1));

  const auto first = engine.tick();
  KCD2S_CHECK(first.status == PlayerCommandRunStatus::WaitingForNativeWake);
  KCD2S_CHECK_EQ(first.popped, std::size_t(1));
  KCD2S_CHECK_EQ(first.revives, std::size_t(1));
  KCD2S_CHECK_EQ(engine.queue.diagnostics().pending_commands, std::size_t(3));
  const auto events = engine.events.size();
  const auto pops = engine.pop_calls;
  for (int tick = 0; tick < 100; ++tick) {
    const auto waiting = engine.tick();
    KCD2S_CHECK(waiting.status == PlayerCommandRunStatus::WaitingForNativeWake);
    KCD2S_CHECK_EQ(waiting.popped, std::size_t(0));
    KCD2S_CHECK_EQ(waiting.revives, std::size_t(0));
  }
  KCD2S_CHECK_EQ(engine.events.size(), events);
  KCD2S_CHECK_EQ(engine.pop_calls, pops);

  engine.actual = BodyState::Awake;
  const auto resumed = engine.tick();
  KCD2S_CHECK(resumed.status == PlayerCommandRunStatus::Drained);
  KCD2S_CHECK_EQ(resumed.applied_stats, std::size_t(2));
  KCD2S_CHECK_EQ(resumed.revives, std::size_t(1));
  KCD2S_CHECK(engine.events[events].kind == EventKind::Stat);
  KCD2S_CHECK(engine.events[events].value == 65);
  KCD2S_CHECK(engine.events[events + 2].kind == EventKind::Revive);
  KCD2S_CHECK(engine.events[events + 4].kind == EventKind::Stat);
  KCD2S_CHECK(engine.events[events + 4].value == 20);
}

KCD2S_TEST(runtime_player_commands, failed_revive_refreshes_partial_work_and_stops_tail) {
  Engine engine;
  engine.admit_revive = false;
  engine.actual = BodyState::KnockedOut;
  KCD2S_CHECK(engine.revive());
  KCD2S_CHECK(engine.stat(50));

  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::Failed);
  KCD2S_CHECK(result.failure == PlayerCommandFailure::ReviveRejected);
  KCD2S_CHECK_EQ(result.popped, std::size_t(1));
  KCD2S_CHECK_EQ(result.revives, std::size_t(1));
  KCD2S_CHECK_EQ(engine.events.size(), std::size_t(2));
  KCD2S_CHECK(engine.events.back().kind == EventKind::Refresh);
  KCD2S_CHECK_EQ(engine.queue.diagnostics().pending_commands, std::size_t(1));
}

KCD2S_TEST(runtime_player_commands, unavailable_stats_drop_but_unobservable_wake_fails) {
  Engine engine;
  engine.actual = BodyState::Unavailable;
  KCD2S_CHECK(engine.stat(50));
  const auto stat = engine.tick();
  KCD2S_CHECK(stat.status == PlayerCommandRunStatus::Drained);
  KCD2S_CHECK_EQ(stat.dropped_stats, std::size_t(1));
  KCD2S_CHECK(engine.events.empty());

  KCD2S_CHECK(engine.revive());
  KCD2S_CHECK(engine.stat(25));
  const auto revive = engine.tick();
  KCD2S_CHECK(revive.status == PlayerCommandRunStatus::Failed);
  KCD2S_CHECK(revive.failure == PlayerCommandFailure::BodyUnavailable);
  KCD2S_CHECK_EQ(engine.queue.diagnostics().pending_commands, std::size_t(1));
}

KCD2S_TEST(runtime_player_commands, body_loss_while_waiting_is_explicit_failure_without_pop) {
  Engine engine;
  engine.actual = BodyState::KnockedOut;
  KCD2S_CHECK(engine.revive());
  KCD2S_CHECK(engine.stat(25));
  KCD2S_CHECK(engine.tick().status == PlayerCommandRunStatus::WaitingForNativeWake);
  const auto pops = engine.pop_calls;
  engine.actual = BodyState::Unavailable;

  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::Failed);
  KCD2S_CHECK(result.failure == PlayerCommandFailure::BodyUnavailable);
  KCD2S_CHECK_EQ(engine.pop_calls, pops);
  KCD2S_CHECK_EQ(engine.queue.diagnostics().pending_commands, std::size_t(1));
}

KCD2S_TEST(runtime_player_commands, stale_scope_after_pop_prevents_native_mutation) {
  Engine engine;
  KCD2S_CHECK(engine.stat(50));
  KCD2S_CHECK(engine.stat(25));
  engine.on_pop = lose_scope;

  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::ScopeChanged);
  KCD2S_CHECK_EQ(result.popped, std::size_t(1));
  KCD2S_CHECK(engine.events.empty());
  KCD2S_CHECK_EQ(engine.observe_calls, std::size_t(0));
  KCD2S_CHECK_EQ(engine.queue.diagnostics().pending_commands, std::size_t(1));
}

KCD2S_TEST(runtime_player_commands, stale_scope_during_observation_prevents_setter) {
  Engine engine;
  KCD2S_CHECK(engine.stat(50));
  engine.on_observe = lose_scope;

  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::ScopeChanged);
  KCD2S_CHECK(engine.events.empty());
}

KCD2S_TEST(runtime_player_commands, body_replacement_during_mutation_prevents_stale_refresh) {
  Engine engine;
  KCD2S_CHECK(engine.stat(50));
  KCD2S_CHECK(engine.revive());
  engine.on_stat = [](Engine& state, const integration::StatRequest&) {
    state.scope.incarnation = 2;
    state.queue.bind(state.scope);
  };
  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::ScopeChanged);
  KCD2S_CHECK_EQ(result.applied_stats, std::size_t(1));
  KCD2S_CHECK_EQ(engine.events.size(), std::size_t(1));
  KCD2S_CHECK_EQ(engine.queue.diagnostics().pending_commands, std::size_t(0));
}

KCD2S_TEST(runtime_player_commands, wake_wait_resets_only_for_a_replacement_scope) {
  Engine engine;
  engine.actual = BodyState::KnockedOut;
  KCD2S_CHECK(engine.revive());
  KCD2S_CHECK(engine.stat(50));
  KCD2S_CHECK(engine.tick().status == PlayerCommandRunStatus::WaitingForNativeWake);
  const auto old = engine.scope;
  engine.scope.incarnation = 2;
  KCD2S_CHECK(engine.queue.bind(engine.scope));
  KCD2S_CHECK(engine.stat(25));
  const auto stale = engine.runner.run_tick(old, engine.callbacks());
  KCD2S_CHECK(stale.status == PlayerCommandRunStatus::ScopeChanged);
  KCD2S_CHECK(engine.runner.waiting_for_native_wake());

  engine.runner.reset();
  engine.actual = BodyState::Awake;
  const auto replacement = engine.tick();
  KCD2S_CHECK(replacement.status == PlayerCommandRunStatus::Drained);
  KCD2S_CHECK_EQ(replacement.applied_stats, std::size_t(1));
  KCD2S_CHECK(engine.applied.front().value == 25);
  KCD2S_CHECK(!engine.runner.waiting_for_native_wake());
}

KCD2S_TEST(runtime_player_commands, continuous_receipts_cannot_exceed_the_per_tick_budget) {
  Engine engine;
  KCD2S_CHECK(engine.stat(50));
  engine.on_stat = replenish_stat;

  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::BudgetExhausted);
  KCD2S_CHECK_EQ(result.popped, std::size_t(64));
  KCD2S_CHECK_EQ(result.applied_stats, std::size_t(64));
  KCD2S_CHECK_EQ(engine.pop_calls, std::size_t(64));
  KCD2S_CHECK_EQ(engine.queue.diagnostics().pending_commands, std::size_t(1));
}

KCD2S_TEST(runtime_player_commands, overflow_poll_exposes_poison_and_loss_counters) {
  Engine engine;
  for (std::size_t i = 0; i < integration::PlayerEngineCommandQueue::kCapacity; ++i) {
    KCD2S_CHECK(engine.stat(50));
  }
  KCD2S_CHECK(!engine.stat(25));
  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::Failed);
  KCD2S_CHECK(result.failure == PlayerCommandFailure::PollRejected);
  KCD2S_CHECK(result.poll_status == integration::PlayerEngineCommandStatus::Overflow);
  KCD2S_CHECK_EQ(result.overflows, std::uint64_t(1));
  KCD2S_CHECK_EQ(result.discarded_commands, std::uint64_t(65));
  KCD2S_CHECK(engine.events.empty());
}

KCD2S_TEST(runtime_player_commands, rejected_or_malformed_polls_fail_without_native_work) {
  for (const auto status : {integration::PlayerEngineCommandStatus::Unbound,
                            integration::PlayerEngineCommandStatus::ScopeMismatch,
                            integration::PlayerEngineCommandStatus::Command}) {
    Engine engine;
    engine.poll_override = integration::PlayerEngineCommandPoll{status};
    const auto result = engine.tick();
    KCD2S_CHECK(result.status == PlayerCommandRunStatus::Failed);
    KCD2S_CHECK(result.poll_status == status);
    KCD2S_CHECK(engine.events.empty());
  }
  Engine missing;
  auto callbacks = missing.callbacks();
  callbacks.observe = nullptr;
  KCD2S_CHECK(missing.runner.run_tick(missing.scope, callbacks).failure ==
              PlayerCommandFailure::MissingCallback);
  KCD2S_CHECK_EQ(missing.pop_calls, std::size_t(0));
}

KCD2S_TEST(runtime_player_commands, mismatched_command_scope_and_revive_owner_are_rejected) {
  for (bool wrong_owner : {false, true}) {
    Engine engine;
    integration::PlayerEngineCommand command;
    command.scope = engine.scope;
    command.sequence = 123;
    command.payload = integration::PlayerReviveCommand{engine.scope.owner};
    if (wrong_owner) {
      command.payload = integration::PlayerReviveCommand{99};
    } else {
      ++command.scope.incarnation;
    }
    engine.poll_override = integration::PlayerEngineCommandPoll{
        integration::PlayerEngineCommandStatus::Command, command};
    const auto result = engine.tick();
    KCD2S_CHECK(result.status == PlayerCommandRunStatus::Failed);
    KCD2S_CHECK(result.failure == PlayerCommandFailure::MalformedCommand);
    KCD2S_CHECK_EQ(result.last_sequence, std::uint64_t(123));
    KCD2S_CHECK(engine.events.empty());
  }
}

KCD2S_TEST(runtime_player_commands, a_knock_out_takes_an_awake_body_and_drops_later_stats) {
  Engine engine;
  KCD2S_CHECK(engine.knock_out(12.5f));
  KCD2S_CHECK(engine.stat(50));
  // A second knock-out finds the body down already.
  KCD2S_CHECK(engine.knock_out(3.0f));
  const auto result = engine.tick();
  KCD2S_CHECK(result.status == PlayerCommandRunStatus::Drained);
  KCD2S_CHECK_EQ(result.knock_outs, std::size_t(1));
  KCD2S_CHECK_EQ(result.dropped_knock_outs, std::size_t(1));
  KCD2S_CHECK_EQ(result.dropped_stats, std::size_t(1));
  KCD2S_CHECK_EQ(engine.events.size(), std::size_t(2));
  KCD2S_CHECK(engine.events[0].kind == EventKind::KnockOut && engine.events[0].value == 12.5f);
  KCD2S_CHECK(engine.events[1].kind == EventKind::Refresh);
}

KCD2S_TEST(runtime_player_commands, a_knock_out_needs_a_positive_health) {
  Engine engine;
  KCD2S_CHECK(!engine.knock_out(0.0f));
  KCD2S_CHECK(!engine.knock_out(-1.0f));
  KCD2S_CHECK_EQ(engine.queue.diagnostics().pending_commands, std::size_t(0));
}
