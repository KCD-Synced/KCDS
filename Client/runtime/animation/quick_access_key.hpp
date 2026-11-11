// The pointing key and the game's own use of it: the game binds B to its quick-access menu
// (the action map "qam_init": a tap draws or holsters the quick-access weapon, a hold opens the
// menu), so pointing also drew the sword. While the player may point, that action map is kept off;
// the game turns it on again on its own (after a menu), so it is switched off again every
// kRecheckMs. Once the server refuses pointing, the map is turned on once and left to the game.
// Pure; pointing.cpp switches the map.
#pragma once

#include <cstdint>

namespace kcds::runtime::animation::quick_access_key {

inline constexpr std::uint64_t kRecheckMs = 250;

enum class Switch : std::uint8_t { None, Off, On };

struct State {
  bool off = false;
  std::uint64_t next = 0;
};

// What to do with the game's quick-access map this frame (`now` in milliseconds).
inline Switch step(State& state, bool may_point, std::uint64_t now) {
  if (may_point) {
    if (state.off && now < state.next) {
      return Switch::None;
    }
    state.off = true;
    state.next = now + kRecheckMs;
    return Switch::Off;
  }
  if (!state.off) {
    return Switch::None;
  }
  state.off = false;
  return Switch::On;
}

}  // namespace kcds::runtime::animation::quick_access_key
