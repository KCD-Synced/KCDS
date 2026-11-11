// The health of the server's NPCs on their bodies.
//
// Every body of an NPC shows the server's `health` and dies when `alive` goes false; no body takes
// health of its own game. The game's blows still land on a body (its hit reaction plays), but the
// damage hooks take the health out of them and report it as an NPC_HIT (npc.hpp): the client of
// the player who struck reports its own blow, the NPC's simulator anything else (a fall, a fire,
// another NPC). Blows of other players' replicas are not reported here: their own clients do.
//
// The soul (actor + 1640, WHGame 1.5.5):
//   soul + 1800      its health (the cache the game's own code reads)
//        a state's setter (soul, state, value): the script binding's SetState; it
//                    leaves health alone at 0
//   soul + 240       its damage interface; vtable slot 27 takes stamina, then health, the
//                    attacker's soul, a flag and a damage type (freed),
//                    as the script's soul:DealDamage calls it. A lethal amount kills the body the
//                    game's own way (an NPC fell dead and lay).
//
// The registry of bodies is written on the engine thread and read by the damage hooks, which may
// run elsewhere; it has its own lock.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "kcd2s/integration/npc.hpp"

namespace kcds::runtime::npcs::health {

struct Natives {
  void* set_state = nullptr;         // SetState
  void* damage_type_init = nullptr;  // a default damage type
  void* damage_type_free = nullptr;  // its release
};

// False when a native is missing.
bool install(const Natives& natives);

// A body of an NPC this client shows.
struct Body {
  std::uint32_t npc = 0;
  // This client simulates the NPC (its body runs the soul's brain).
  bool simulated = false;
};

void track(std::uint32_t engine_id, void* soul, const Body& body);
void forget(std::uint32_t npc);
void clear();
std::optional<Body> body_of_entity(std::uint32_t engine_id);
std::optional<Body> body_of_soul(const void* soul);

// The share of their damage an NPC's blows on players take (npc.hpp NpcDamage,
// Npc.setCombatSkill({ damage })), as the server last said; the game's own (1) before that.
void set_damage_share(std::uint32_t npc, float share);
float damage_share(std::uint32_t npc);

// Whether players can fight the NPC (npc.hpp NpcAttackable, Npc.attackable).
void set_attackable(std::uint32_t npc, bool attackable);
bool attackable(std::uint32_t npc);
// Whether players can talk to the NPC (npc.hpp NpcInteractable, Npc.interactable).
void set_interactable(std::uint32_t npc, bool interactable);
bool interactable(std::uint32_t npc);

// Whether the HUD names the NPC when a player looks at it (npc.hpp NpcNameShown, Npc.nameShown),
// as the server last said; true before that. By entity: true for any entity of no NPC.
void set_name_shown(std::uint32_t npc, bool shown);
bool name_shown(std::uint32_t npc);
bool name_shown_of_entity(std::uint32_t engine_id);

// Whether a player can lock on to the NPC (npc.hpp NpcTargetable, Npc.targetable), as the
// server last said; true before that.
void set_targetable(std::uint32_t npc, bool targetable);
bool targetable(std::uint32_t npc);

// Whether a dead animal offers neither the game's Search nor its Gut (npc.hpp NpcCarcassClosed),
// as the server last said; false before that.
void set_carcass_closed(std::uint32_t npc, bool closed);
bool carcass_closed(std::uint32_t npc);

// What dealt health a game would have taken off a body.
enum class Source {
  LocalPlayer,
  Npc,
  RemotePlayer,
  World,
};

// The NPC_HIT to send for that health, or none: the local player's blow always, the rest only
// from the NPC's simulator, and nothing for another player's replica (its own client reports
// it) or for nothing taken. `attacker` is the local player's or the NPC's network id.
std::optional<kcd2s::integration::NpcHit> hit_to_report(const Body& body, Source source,
                                                        std::uint32_t attacker, float amount,
                                                        std::uint8_t reason, std::int32_t subpart);

// Hits the hooks found, sent on the engine thread.
void queue_hit(const kcd2s::integration::NpcHit& hit);
std::vector<kcd2s::integration::NpcHit> take_hits();

// Blows the hooks found for the game on the other side of a fight (NPC_BLOW): the local player's
// on a replica of an NPC, for its simulator; a simulated NPC's on another player's replica, for
// that player's own game. Sent on the engine thread.
void queue_blow(const kcd2s::integration::NpcBlow& blow);
std::vector<kcd2s::integration::NpcBlow> take_blows();

// The soul's health.
float health_of(const void* soul);
// Shows the server's health (above 0) on the soul when it differs.
void show_health(void* soul, float health);
// Kills the body with the game's own lethal damage.
void kill(void* soul);
// True on this thread while kill() deals its damage: the damage hooks let it through.
bool killing();

}  // namespace kcds::runtime::npcs::health
