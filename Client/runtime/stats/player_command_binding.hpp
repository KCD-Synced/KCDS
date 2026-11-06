// The server's ordered player commands (setStat, revive and the like) bound to the local player's
// current body; run_player_commands is declared in core/client_internal.hpp.
#pragma once

#include <cstdint>

#include "kcd2s/core/ids.hpp"
#include "player_commands.hpp"

namespace kcds::game_client {

// The local player's body the server's player commands run on (its scope and session).
struct PlayerCommandBinding {
  kcd2s::integration::PlayerEngineScope scope;
  void* actor = nullptr;
  void* soul = nullptr;
  std::uint64_t session = 0;
  std::uint64_t observed_overflows = 0;
  bool failed = false;
  // The body lies as the game's sleeping ragdoll since setHealth(0, { fall: true }) until a revive
  // makes it alive again.
  bool fell_asleep = false;
  kcds::runtime::stats::PlayerCommandRunner runner;
};
extern PlayerCommandBinding player_command_binding;

void reset_player_command_binding();
bool bind_player_command_body(void* actor, std::uint32_t revision, kcd2s::core::EntityId owner);

}  // namespace kcds::game_client
