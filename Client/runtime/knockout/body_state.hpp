#pragma once

namespace kcds::runtime::knockout {

enum class BodyState {
  Unavailable,
  Awake,
  KnockedOut,
};

using IsUnconscious = bool(__fastcall*)(void* soul);

// Pure engine-thread observation. The caller must have reacquired and validated tracked_soul
// from the current owned native body; no historical pointer may be used merely to test it.
// Refuses identity/interface mismatches, non-finite HP and nonpositive HP before calling the
// uniquely resolved native predicate. Never reads a raw consciousness cache or changes native
// state, buffs, UI or the knockout module's prior-tick state.
BodyState observe_body(void* soul, void* tracked_soul, IsUnconscious uniquely_resolved);

}  // namespace kcds::runtime::knockout
