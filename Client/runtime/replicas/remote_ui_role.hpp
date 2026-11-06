// Only the missile UI listener belonging to the remote C_Player currently being constructed
// is local-only. Other registrations, including nested local construction, keep native behavior.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::replicas {

inline constexpr std::size_t kPlayerMissileUiListener = 3448;
inline thread_local const void* constructing_remote_player = nullptr;

inline const void* begin_remote_player_construction(const void* player, bool remote) noexcept {
  const auto* previous = constructing_remote_player;
  constructing_remote_player = remote ? player : nullptr;
  return previous;
}

inline void end_remote_player_construction(const void* previous) noexcept {
  constructing_remote_player = previous;
}

// Portable C++ scope for normal calls/tests. The native constructor wrapper uses these same
// state transitions with __finally: /EHsc does not run this destructor for a native SEH fault.
class RemotePlayerConstruction {
 public:
  RemotePlayerConstruction(const void* player, bool remote)
      : previous_(begin_remote_player_construction(player, remote)) {
  }

  ~RemotePlayerConstruction() {
    end_remote_player_construction(previous_);
  }

  RemotePlayerConstruction(const RemotePlayerConstruction&) = delete;
  RemotePlayerConstruction& operator=(const RemotePlayerConstruction&) = delete;

 private:
  const void* previous_;
};

template <class Forward, class Consume>
bool route_ui_registration(const void* listener, const Forward& forward, const Consume& consume) {
  const auto player = reinterpret_cast<std::uintptr_t>(constructing_remote_player);
  if (player && reinterpret_cast<std::uintptr_t>(listener) == player + kPlayerMissileUiListener) {
    consume();
    return false;
  }
  forward();
  return true;
}

}  // namespace kcds::runtime::replicas
