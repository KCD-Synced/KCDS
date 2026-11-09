#include "player_commands.hpp"

namespace kcds::runtime::stats {

namespace {

namespace integration = kcd2s::integration;
using knockout::BodyState;

bool complete(const PlayerCommandCallbacks& callbacks) {
  return callbacks.pop && callbacks.is_current && callbacks.observe && callbacks.apply_stat &&
         callbacks.revive && callbacks.knock_out && callbacks.after_mutation;
}

bool current(const PlayerCommandCallbacks& callbacks,
             const integration::PlayerEngineScope& expected) {
  return callbacks.is_current(callbacks.context, expected);
}

PlayerCommandRunResult failed(PlayerCommandRunResult result, PlayerCommandFailure failure) {
  result.status = PlayerCommandRunStatus::Failed;
  result.failure = failure;
  return result;
}

PlayerCommandRunResult scope_changed(PlayerCommandRunResult result) {
  result.status = PlayerCommandRunStatus::ScopeChanged;
  return result;
}

}  // namespace

void PlayerCommandRunner::reset() {
  waiting_for_native_wake_ = false;
}

bool PlayerCommandRunner::waiting_for_native_wake() const {
  return waiting_for_native_wake_;
}

PlayerCommandRunResult PlayerCommandRunner::run_tick(const integration::PlayerEngineScope& scope,
                                                     const PlayerCommandCallbacks& callbacks) {
  PlayerCommandRunResult result;
  if (!complete(callbacks)) {
    return failed(result, PlayerCommandFailure::MissingCallback);
  }
  // The caller may replace its current scope inside a callback. Keep this drain's identity
  // independent even if the caller passed that same object by reference.
  const auto expected = scope;
  if (!current(callbacks, expected)) {
    return scope_changed(result);
  }
  if (waiting_for_native_wake_) {
    const auto body = callbacks.observe(callbacks.context);
    if (!current(callbacks, expected)) {
      return scope_changed(result);
    }
    if (body == BodyState::Unavailable) {
      return failed(result, PlayerCommandFailure::BodyUnavailable);
    }
    if (body == BodyState::KnockedOut) {
      result.status = PlayerCommandRunStatus::WaitingForNativeWake;
      return result;
    }
    waiting_for_native_wake_ = false;
  }

  for (std::size_t count = 0; count < kCommandsPerTick; ++count) {
    if (!current(callbacks, expected)) {
      return scope_changed(result);
    }
    auto poll = callbacks.pop(callbacks.context, expected);
    result.poll_status = poll.status;
    result.overflows = poll.overflows;
    result.discarded_commands = poll.discarded_commands;
    if (poll.status == integration::PlayerEngineCommandStatus::Empty) {
      return result;
    }
    if (poll.status != integration::PlayerEngineCommandStatus::Command) {
      return failed(result, PlayerCommandFailure::PollRejected);
    }
    if (!poll.command) {
      return failed(result, PlayerCommandFailure::MalformedCommand);
    }
    ++result.popped;
    result.last_sequence = poll.command->sequence;
    if (!(poll.command->scope == expected)) {
      return failed(result, PlayerCommandFailure::MalformedCommand);
    }
    if (!current(callbacks, expected)) {
      return scope_changed(result);
    }
    if (const auto* request = std::get_if<integration::StatRequest>(&poll.command->payload)) {
      if (!integration::stat_request_valid(*request)) {
        return failed(result, PlayerCommandFailure::MalformedCommand);
      }
      const auto body = callbacks.observe(callbacks.context);
      if (!current(callbacks, expected)) {
        return scope_changed(result);
      }
      if (body != BodyState::Awake) {
        ++result.dropped_stats;
        continue;
      }
      callbacks.apply_stat(callbacks.context, *request);
      ++result.applied_stats;
    } else if (const auto* knock =
                   std::get_if<integration::PlayerKnockOutCommand>(&poll.command->payload)) {
      // Only an awake body is knocked out; one that is down already stays as it is.
      const auto body = callbacks.observe(callbacks.context);
      if (!current(callbacks, expected)) {
        return scope_changed(result);
      }
      if (body != BodyState::Awake) {
        ++result.dropped_knock_outs;
        continue;
      }
      if (callbacks.knock_out(callbacks.context, knock->health)) {
        ++result.knock_outs;
      } else {
        ++result.dropped_knock_outs;
      }
    } else {
      const auto& revive = std::get<integration::PlayerReviveCommand>(poll.command->payload);
      if (revive.player != expected.owner) {
        return failed(result, PlayerCommandFailure::MalformedCommand);
      }
      const bool admitted = callbacks.revive(callbacks.context);
      ++result.revives;
      if (!current(callbacks, expected)) {
        return scope_changed(result);
      }
      callbacks.after_mutation(callbacks.context);
      if (!current(callbacks, expected)) {
        return scope_changed(result);
      }
      if (!admitted) {
        return failed(result, PlayerCommandFailure::ReviveRejected);
      }
      waiting_for_native_wake_ = true;
      const auto body = callbacks.observe(callbacks.context);
      if (!current(callbacks, expected)) {
        return scope_changed(result);
      }
      if (body == BodyState::Unavailable) {
        return failed(result, PlayerCommandFailure::BodyUnavailable);
      }
      if (body == BodyState::KnockedOut) {
        result.status = PlayerCommandRunStatus::WaitingForNativeWake;
        return result;
      }
      waiting_for_native_wake_ = false;
      continue;
    }
    if (!current(callbacks, expected)) {
      return scope_changed(result);
    }
    callbacks.after_mutation(callbacks.context);
    if (!current(callbacks, expected)) {
      return scope_changed(result);
    }
  }
  result.status = PlayerCommandRunStatus::BudgetExhausted;
  return result;
}

}  // namespace kcds::runtime::stats
