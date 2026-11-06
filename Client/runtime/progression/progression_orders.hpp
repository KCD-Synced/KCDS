// The server's progression orders for the local player:
// Player.addXp, setLevel, addPerk, removePerk, addPerkPoints, resetPerks and
// loadProgression, and the perk gate's answers, applied through the game's own paths
// (WHGame 1.5.5):
//
//   XP          the RPG module's own path, as the script binding's AddSkillXP / AddStatXP do:
//               a script cause (on the soul's id at soul + 48), a C_SkillXPEffect
//               or C_StatXPEffect on it with the XP in fixed point
//               (128 per XP), the player's XP multipliers on or off (the effect's flag at + 56
//               for a skill, + 52 for a stat), submitted to the RPG module's queue (the global
// + 0xB0, vtable slot 0). The game applies it: level-ups, their
//               perk points and the level-up notice.
//   a level     the XP between the two levels, as AdvanceToSkillLevel / AdvanceToStatLevel
//               work it out (the game's totals,on its RPG parameters)
//   perks       the soul's perk set (soul + 1880): add and remove
//   points      the perk set's counters (its + 88: stats + 16 + 2 * stat, skills + 36 + 2 *
//               skill, the main level + 106), raised as the game's level-up listener raises them,
//               then the game's perk update
//   respec      the game's own respec buff (rpg/buff.xml "respec", Cpp:Respec)
//   learn       the game's own learn of a perk the player asked for (perk_gate::learn)
//   block list  the perks the game may not grant by itself (perk_gate::set_blocked)
//   refusal     a perk the player asked for and the server turned down (perk_gate::refused)
//   restore     the XP each track lacks (no multipliers); once the game's perk-point listener
//               has taken each of those effects (for skills, for stats,
//               hooked: it adds the points of the levels crossed after the XP itself landed),
//               the missing perks and the snapshot's unspent points
//   set track   the soul's own level / XP pair of the track (skills at soul + 1512,
//               stats at soul + 1388, 8 bytes each: level, XP towards the next in fixed point)
//               written as it is, then what the game's XP add does after it changes the pair,
//               without the XP effect and so without its listeners (perk points, the level-up
//               notice): for a skill the skill set's change (the "skill changed"
//               listeners at the soul's + 88, then the induced skill fencing rebuilt from the
//               weapon skills), for a stat the stat's change (its
//               listeners, the derived stats)
//   exact       a restore with exact: every track written as set track writes it, the perks
//   restore     it owns beyond the snapshot removed and the missing ones added, the unspent
//               points set
//
// Engine thread.
#pragma once

#include <vector>

#include "kcd2s/integration/progression.hpp"

namespace kcds::runtime::progression {

using Logger = void (*)(const char* format, ...);

struct OrderNatives {
  void* const* rpg = nullptr;     // the RPG module
  void* create_cause = nullptr;   // a script cause on a soul
  void* release_cause = nullptr;  // its handle's release
  void* skill_xp = nullptr;       // a C_SkillXPEffect
  void* stat_xp = nullptr;        // a C_StatXPEffect
  void* rpg_params = nullptr;     // the RPG parameters
  void* skill_total = nullptr;    // a skill's total XP to a level
  void* stat_total = nullptr;     // a stat's
  void* add_perk = nullptr;
  void* remove_perk = nullptr;
  void* update_perks = nullptr;
  void* buff_manager = nullptr;   // the buff manager's getter
  void* skill_points = nullptr;   // the perk-point listener of skill XP, hooked
  void* stat_points = nullptr;    // and of stat XP, hooked
  void* skill_changed = nullptr;  // the skill set's change of a skill
  void* stat_changed = nullptr;   // a stat's change
};

// False when a native is missing or a hook fails.
bool install(const OrderNatives& natives, Logger logger);

// Applies the server's orders to the local player's soul (nullptr: they are dropped) and goes on
// with a restore the game is still applying. Every frame.
void apply(void* soul, const std::vector<kcd2s::integration::ProgressionOrder>& orders);

// Engine-thread replica adapter only. Mirrors the owner's observed Riposte
// ability through original perk add/remove paths, without opening a slot.
bool mirror_riposte_ability(void* replica_soul, bool enabled);

// Whether the server lets the player's own game earn XP (its last NativeXp order; true until one
// arrives and again outside a world).
bool native_xp();

}  // namespace kcds::runtime::progression
