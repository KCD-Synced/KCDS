// Harm of the local player that is not another player's blow (an NPC's blow, a fall, fire, a
// collision), held for the server's ruling while some resource handles playerHarm (HIT_RULES): the
// game's damage packet does not reach the body; PLAYER_HARM asks the server, and its HIT_VERDICT
// takes the health it ruled through the game's own state setter (none when refused). One with no
// ruling after kHarmRulingMs is taken as the game would have. Damage ticks (bleeding, poison,
// hunger) and the server's own changes are never held. Engine thread.
#pragma once

#include <vector>

#include "kcd2s/integration/damage.hpp"

namespace kcds::game_client {

// Whether the server holds harm now (set by the combat tick from HIT_RULES).
void set_harm_rules(bool held);

// The game is about to run a call that may take health off the local player (its damage
// function, a health effect such as a fall's): true when what the call takes is held for a
// ruling. The call's health writes then go to block_harm_write (true: the write is kept back),
// and end_harm_hold after the call asks the server about what it wanted to take, with the reason
// and the attacker (a soul, or null) as the game settled them.
bool begin_harm_hold(std::uint32_t reason, void* attacker);
bool block_harm_write(void* soul, float current, float value);

// The game queued a hit on the local player from `source_entity` that is not another player's:
// true when its health effect will be held (and reported once taken), so the hit is not reported
// by itself.
bool harm_hit_queued(std::uint32_t source_entity);
void end_harm_hold(std::uint32_t reason, std::int32_t subpart, void* attacker);

// Takes the rulings on held harm out of `verdicts` and applies them; the rest stay.
void rule_held_harm(std::vector<kcd2s::integration::HitVerdict>& verdicts);

// Every frame: harm with no ruling for too long is taken as the game would have.
void tick_held_harm();

// The local player's body changed or the world reset: what was held goes.
void discard_held_harm(const char* reason);

}  // namespace kcds::game_client
