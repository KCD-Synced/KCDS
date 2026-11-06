// Clinches on every screen: the game's own clinch start, a new clinch decided by the
// handshake between the two clients, and the started clinch relayed to every watcher.
#pragma once

#include <cstddef>
#include <cstdint>

#include "clinch_handshake.hpp"
#include "kcd2s/core/ids.hpp"
#include "kcd2s/integration/combat.hpp"

namespace kcds::game_client {

// every clinch's start (hooked: the original), and the native clinch kinds.
using StartClinch = char(__fastcall*)(unsigned, void*, void*, void*, void*);
extern StartClinch original_start_clinch;
extern const std::uint32_t* native_clinch_kinds[2];
char __fastcall hooked_start_clinch(unsigned kind, void* initiator, void* other, void* guard,
                                    void* other_guard);
// the approach precondition, and, whether a clinch state is one.
using ClinchReady = bool(__fastcall*)(void*);
extern ClinchReady native_clinch_ready;
using MovementCheck = bool(__fastcall*)(void* movement);
extern MovementCheck native_in_clinch_state;
extern bool applying_relayed_clinch;
extern kcds::runtime::combat::ClinchHandshake clinch_handshake;

// A fighter's combat-model int property (its value at + 8), -1 for none.
int combat_property_value(void* movement, std::size_t property);
bool locked_on_each_other(void* first, void* second);
int native_clinch_code(unsigned kind);
void send_clinch(kcd2s::core::EntityId target, std::uint8_t mode, int code);
void relay_started_clinch(kcd2s::core::EntityId other, int code);
void apply_watched_clinch(kcd2s::core::EntityId source, void* initiator, void* own_move, int code,
                          unsigned kind);
void apply_relayed_clinch(kcd2s::core::EntityId source, void* replica_actor, void* local_actor,
                          const kcd2s::integration::CombatActionData& data);

}  // namespace kcds::game_client
