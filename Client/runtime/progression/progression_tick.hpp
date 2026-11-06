// The local player's progression each frame: the server's orders,
// the reports to it, and the learn-perk test hook.
#pragma once

#include <cstdint>
#include <string>

#include "kcd2s/integration/progression.hpp"

namespace kcds::game_client {

// What the perk screen's learn helper asks for the soul: an object whose vtable slot 0 returns
// a pointer to the soul's id.
struct TestSoulSource {
  void* const* vtable;
  std::uint64_t soul_id;
};
using ScreenLearnPerk = double(__fastcall*)(void* unused, TestSoulSource* soul,
                                            const kcd2s::integration::PerkId* perk, char learn);
// The perk screen's learn helper (resolved by the setup; the learn-perk test calls it).
extern ScreenLearnPerk screen_learn_perk;

// The server's progression orders for the local player.
void tick_progression_orders(void* local_actor);
// Reports the local player's progression to the server when it changed.
void tick_progression(void* local_actor);
// The local player's replicaSoul report, read every 250 ms.
const std::string& tick_replica_soul(void* local_actor);

}  // namespace kcds::game_client
