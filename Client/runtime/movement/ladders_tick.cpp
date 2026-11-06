// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "ladders_tick.hpp"

#include <array>
#include <utility>
#include <vector>

#include "../core/natives.hpp"
#include "../input/input_gate.hpp"
#include "shared_ladders.hpp"

namespace kcds::game_client {

void tick_ladders(const Frame& frame) {
  const auto local_actor = frame.local_actor;
  const auto local_entity = frame.local_entity;
  std::vector<kcds::runtime::movement::ladders::Climber> climbers;
  for (const auto& [id, position] : ladder_climbers) {
    climbers.push_back({std::uint32_t(id), position});
  }
  std::array<float, 3> position{};
  const float* local_tm =
      local_entity ? slot<const float*(__fastcall*)(void*)>(local_entity, 32)(local_entity)
                   : nullptr;
  if (local_tm) {
    position = {local_tm[3], local_tm[7], local_tm[11]};
  }
  const bool on_ladder =
      local_actor && slot<bool(__fastcall*)(void*)>(local_actor, kActorIsOnLadder)(local_actor);
  kcds::runtime::movement::ladders::tick(local_actor, position, on_ladder, std::move(climbers),
                                         hooked_player_input_action);
}

}  // namespace kcds::game_client
