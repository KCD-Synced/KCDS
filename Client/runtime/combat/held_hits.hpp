// Blows and missiles on the local player held until the server rules on them, the
// game's own pricing of melee blows (priced hits), and the queue of native hits on the local
// player. original_queue_native_hit and the native wake request are declared in
// core/client_internal.hpp.
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "kcd2s/integration/combat.hpp"
#include "kcd2s/integration/damage.hpp"

namespace kcds::game_client {

// The ruling on the blow the game is working out now: set around a ruled contact's
// evaluation, by the relayed contact or its physical replay, both of which queue the
// blow's hit. The hit queue hook puts the ruled health in the hit record (which playerDamage
// reports) and keeps what the game worked out. Engine thread.
struct ActiveRuling {
  kcd2s::integration::HitVerdict verdict;
  float worked_out = 0;
  float ruled = 0;
  bool queued = false;  // the blow's hit on the local player was queued
};
using PriceBlow = void*(__fastcall*)(void*, void*, void*);
using CapPrice = void(__fastcall*)(void*);
using ApplyHealthEffect = char(__fastcall*)(void*);
using CalculatorItem = void*(__fastcall*)(void*);
using KnockOut = void*(__fastcall*)(void*, void*, float);
using CombatConfig = std::uint8_t*(__fastcall*)();
using PlayerContactAllowed = char(__fastcall*)(void*, void*);

extern std::optional<ActiveRuling> active_ruling;
bool hold_contact(const kcd2s::integration::MeleeEvent& impact);
std::vector<std::pair<kcd2s::integration::MeleeEvent, kcd2s::integration::HitVerdict>>
take_ruled_contacts(const std::vector<kcd2s::integration::HitVerdict>& verdicts);
// Whether blows wait for the server's ruling (the server's HIT_RULES).
extern std::atomic<bool> hits_ruled;
void rule_missiles(const std::vector<kcd2s::integration::HitVerdict>& verdicts);

// The game's pricing of a melee blow, its health effect and knock-out (hooked: the originals).
extern PriceBlow original_price_blow;
extern CapPrice cap_price;
extern ApplyHealthEffect original_apply_health_effect;
extern CalculatorItem calculator_item_32;
extern CalculatorItem calculator_item_48;
extern KnockOut original_knock_out;
extern CombatConfig combat_config;
extern bool priced_hits_ready;
void* __fastcall hooked_price_blow(void* calculator, void* result, void* input);
char __fastcall hooked_apply_health_effect(void* effect);
void* __fastcall hooked_knock_out(void* combat_soul, void* event, float duration);
void rule_priced_blows(const std::vector<kcd2s::integration::HitVerdict>& verdicts);

// The player contact check (hooked), and the queue of native hits on the local player.
extern PlayerContactAllowed original_player_contact_allowed;
char __fastcall hooked_player_contact_allowed(void* movement, void* target);
std::intptr_t __fastcall hooked_queue_native_hit(void* rules, const void* info);

}  // namespace kcds::game_client
