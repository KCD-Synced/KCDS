// Robbing another player with the game's own pickpocketing,
// WHGame 1.5.5.
//
// The game offers Rob on an NPC from its script (BasicAIActions.GetActions: the hint
// @ui_hud_basic_steal, held, then human:RequestPickpocketing); a replica is the Player class,
// whose script offers none. The client offers the same hint itself when the local player stands
// close behind another player's replica and faces it, and holding it starts the game's own
// pickpocketing against the replica (C_ScriptBindHuman::RequestPickpocketing's native,
// the thief's actor, the victim's actor): the charge minigame, the victim's noticing
// and the loot screen are the game's. The server decides first (a PICKPOCKET request answered by
// a PICKPOCKET_RULE) and hears how the theft ended: the HUD's pickpocketing result
// (the notification manager, the result: 0 interrupted, 1 succeeded, 2 noticed by the victim,
// 3 lost, 5 nothing stolen, 6 seen by someone) is hooked. A victim who caught a thief gets the
// server's text as the game's own HUD notification (the notification manager's interface, kept
// by the UI service at + 344 then + 96: slot 0 takes a game string).
//
// Engine thread.
#ifndef KCDS_RUNTIME_PICKPOCKET_PICKPOCKET_HPP
#define KCDS_RUNTIME_PICKPOCKET_PICKPOCKET_HPP

#include <cstdint>
#include <vector>

#include "kcd2s/integration/pickpocket.hpp"

namespace kcds::runtime::pickpocket {

using Logger = void (*)(const char* format, ...);

struct Natives {
  // The game's start of pickpocketing (thief actor, victim actor).
  void* start = nullptr;
  // The HUD's pickpocketing result, hooked.
  void* result = nullptr;
  // The UI service holding the notification manager.
  void* notifications = nullptr;
  // The game's strings: from text (CryString constructor), freed.
  void* string_from_text = nullptr;
  void* string_free = nullptr;
  // The game's base check whether an item is stolen (item; the HUD's red hand, shops and
  // the full check all ask it first), hooked: with stolen marks off
  // (Inventory.setStolenMarks) no item is stolen.
  void* is_stolen = nullptr;
  // The script binding human:CanBeRobbed (C_ScriptBindHuman slot 40: binding, call
  // arguments), which the NPC's script asks before it offers Rob (BasicAIActions.GetActions),
  // hooked: a server NPC is never offered (Rob was still offered in front of one). With it:
  // the binding's victim actor (binding, arguments) and its boolean return to the
  // script (arguments, the value).
  void* can_be_robbed = nullptr;
  void* robbed_actor = nullptr;
  void* return_bool = nullptr;
  // Sends a report to the server; false when it could not go.
  bool (*send_report)(const kcd2s::integration::PickpocketReport& report) = nullptr;
};

// False when a native is missing.
bool install(const Natives& natives, Logger logger);

// A body in the world: its actor, where it stands and the way it faces (unit, on the ground).
struct Body {
  std::uint32_t player = 0;
  void* actor = nullptr;
  float position[3] = {};
  float forward[2] = {};
};

// Every frame: the local player and the other players' replicas in view. Offers Rob on the
// nearest replica the local player stands behind and faces, and takes it away otherwise.
void tick(const Body& local, const std::vector<Body>& replicas, bool allowed);

// A rule the server sent: whether Rob is offered, the answer to a request, a caught thief.
void apply_rule(const kcd2s::integration::PickpocketRule& rule);

// The actors of the server's NPCs: the game's own pickpocketing of one never starts.
// Its body on this client may have no senses (a replica's runs no AI: it watched a thief rob it
// from the front, live), and what it carries is the server's.
void set_unrobbable(std::vector<void*> actors);

}  // namespace kcds::runtime::pickpocket

#endif  // KCDS_RUNTIME_PICKPOCKET_PICKPOCKET_HPP
