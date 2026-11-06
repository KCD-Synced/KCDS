#include "body_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "kcd2s/testkit.hpp"

namespace {

namespace knockout = kcds::runtime::knockout;
using knockout::BodyState;

unsigned predicate_calls = 0;
unsigned other_calls = 0;
bool native_unconscious = false;
void* observed_soul = nullptr;

bool __fastcall is_unconscious(void* soul) {
  ++predicate_calls;
  observed_soul = soul;
  return native_unconscious;
}

bool __fastcall other_interface(void*) {
  ++other_calls;
  return true;
}

struct Soul {
  alignas(16) std::array<std::uint8_t, 2048> bytes{};
  std::array<void*, 8> table{};

  Soul() {
    table[7] = reinterpret_cast<void*>(is_unconscious);
    void** pointer = table.data();
    std::memcpy(bytes.data(), &pointer, sizeof(pointer));
    health(100);
  }

  void* data() {
    return bytes.data();
  }

  void health(float value) {
    std::memcpy(bytes.data() + 1800, &value, sizeof(value));
  }
};

void reset_predicate() {
  predicate_calls = 0;
  other_calls = 0;
  native_unconscious = false;
  observed_soul = nullptr;
}

}  // namespace

KCD2S_TEST(native_body_state, null_and_untracked_objects_are_rejected_without_native_access) {
  reset_predicate();
  Soul tracked;
  KCD2S_CHECK(knockout::observe_body(nullptr, tracked.data(), is_unconscious) ==
              BodyState::Unavailable);
  KCD2S_CHECK(knockout::observe_body(tracked.data(), nullptr, is_unconscious) ==
              BodyState::Unavailable);
  // An identity mismatch must be rejected before even reading the candidate's interface.
  auto untracked = reinterpret_cast<void*>(std::uintptr_t(1));
  KCD2S_CHECK(knockout::observe_body(untracked, tracked.data(), is_unconscious) ==
              BodyState::Unavailable);
  KCD2S_CHECK_EQ(predicate_calls, 0u);
  KCD2S_CHECK_EQ(other_calls, 0u);
}

KCD2S_TEST(native_body_state, missing_or_mismatched_native_interface_is_not_called) {
  reset_predicate();
  Soul soul;
  KCD2S_CHECK(knockout::observe_body(soul.data(), soul.data(), nullptr) == BodyState::Unavailable);
  soul.table[7] = reinterpret_cast<void*>(other_interface);
  KCD2S_CHECK(knockout::observe_body(soul.data(), soul.data(), is_unconscious) ==
              BodyState::Unavailable);
  soul.table[7] = nullptr;
  KCD2S_CHECK(knockout::observe_body(soul.data(), soul.data(), is_unconscious) ==
              BodyState::Unavailable);
  void** missing = nullptr;
  std::memcpy(soul.data(), &missing, sizeof(missing));
  KCD2S_CHECK(knockout::observe_body(soul.data(), soul.data(), is_unconscious) ==
              BodyState::Unavailable);
  KCD2S_CHECK_EQ(predicate_calls, 0u);
  KCD2S_CHECK_EQ(other_calls, 0u);
}

KCD2S_TEST(native_body_state, nonfinite_and_nonpositive_health_is_never_awake) {
  reset_predicate();
  Soul soul;
  const std::array<float, 6> invalid{0,
                                     -0.0f,
                                     -1,
                                     std::numeric_limits<float>::infinity(),
                                     -std::numeric_limits<float>::infinity(),
                                     std::numeric_limits<float>::quiet_NaN()};
  for (float value : invalid) {
    soul.health(value);
    KCD2S_CHECK(knockout::observe_body(soul.data(), soul.data(), is_unconscious) ==
                BodyState::Unavailable);
  }
  KCD2S_CHECK_EQ(predicate_calls, 0u);
}

KCD2S_TEST(native_body_state, positive_health_still_uses_the_actual_native_ko_predicate) {
  reset_predicate();
  Soul soul;
  soul.health(1);
  native_unconscious = true;
  KCD2S_CHECK(knockout::observe_body(soul.data(), soul.data(), is_unconscious) ==
              BodyState::KnockedOut);
  KCD2S_CHECK_EQ(predicate_calls, 1u);
  KCD2S_CHECK(observed_soul == soul.data());
}

KCD2S_TEST(native_body_state, native_wake_recheck_does_not_reuse_a_cached_knockout_answer) {
  reset_predicate();
  Soul soul;
  soul.health(25);
  native_unconscious = true;
  KCD2S_CHECK(knockout::observe_body(soul.data(), soul.data(), is_unconscious) ==
              BodyState::KnockedOut);
  // The cached previous tick can still say down after native revival. Each observation must
  // call the real predicate again; neither current HP nor that previous answer supplies it.
  native_unconscious = false;
  KCD2S_CHECK(knockout::observe_body(soul.data(), soul.data(), is_unconscious) == BodyState::Awake);
  native_unconscious = true;
  KCD2S_CHECK(knockout::observe_body(soul.data(), soul.data(), is_unconscious) ==
              BodyState::KnockedOut);
  KCD2S_CHECK_EQ(predicate_calls, 3u);
}

KCD2S_TEST(native_body_state, observation_has_no_native_state_or_buff_writes_and_no_health_floor) {
  reset_predicate();
  Soul soul;
  soul.health(0.001f);
  const auto before = soul.bytes;
  KCD2S_CHECK(knockout::observe_body(soul.data(), soul.data(), is_unconscious) == BodyState::Awake);
  KCD2S_CHECK(soul.bytes == before);
  KCD2S_CHECK_EQ(predicate_calls, 1u);
}
