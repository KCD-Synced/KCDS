// XP from play asks the server first: the server decides what a
// player's own game may grant (playerXpGaining, Progression.setXpGain, setXpCap, setMaxLevel,
// setGameXp), so a player cannot earn what the server does not allow.
//
// Every XP gain of the game is an RPG effect applied to a soul (WHGame 1.5.5): C_SkillXPEffect
// (Apply = its vtable slot 3) and C_StatXPEffect; the effect holds the
// soul's id at + 16, the skill or stat at + 24, the XP before the player's multipliers at + 28
// (fixed point, 128 per XP), whether the multipliers apply at + 56 (skill) / + 52 (stat), and its
// event at
// + 8 (a C_Event, whose + 8 is the cause; the cause's class names what produced the XP:
// C_AttackCause is "Attack").
//
// Both Apply functions are hooked. An effect on the local player's soul that the server did not
// order is not applied: it is held back as an XpGain, and the server grants what it allows with an
// AddXp order (Client/runtime/progression/progression_orders), whose effects go through. With
// XP from play switched off by the server, the gains are dropped without asking. A held-back
// effect still answers "applied" (as the game's Apply does when it grants nothing): the game
// applies an event's effects together and refuses the whole event when one fails, so a failing
// XP effect refused the action that earned it, such as a vault.
//
// The hooks run on the thread the game applies RPG effects on (the engine thread).
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/progression.hpp"

namespace kcds::runtime::progression::xp_gate {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* skill_apply = nullptr;  // C_SkillXPEffect::Apply, hooked
  void* stat_apply = nullptr;   // C_StatXPEffect::Apply, hooked
  std::uintptr_t image = 0;     // WHGame.dll's base, for the causes' class names
};

// False when a native is missing or a hook fails.
bool install(const Natives& natives, Logger logger);

// Every frame: the local player's soul (nullptr outside a server world: nothing is held back)
// and whether XP from play may be earned at all.
void tick(void* soul, bool native_xp);

// An effect the server ordered: it is applied without asking.
void allow(void* effect);

// The gains held back since the last call, oldest first.
std::vector<kcd2s::integration::XpGain> take_gains();

}  // namespace kcds::runtime::progression::xp_gate
