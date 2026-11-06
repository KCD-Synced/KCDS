// The game's own "Talk" and focus chat prompts on people (the server's Interactions.set and
// Player.setInteraction: npcTalk, npcChat, playerTalk). The NPC interaction script
// (Scripts/Entities/AI/Shared/BasicAIActions.lua GetActions and GetChatActions) shows them while
// the person's actor answers CanTalk and CanChat, the human's own methods (WHGame 1.5.5:
// C_Human slot 163, CanTalk(this, user id, context), and slot 164,
// CanChat(this, user id)). Hooked: a server NPC's body answers no while the server has
// not allowed npcTalk (or npcChat), another player's replica while it has not allowed
// playerTalk; everything else is the game's own answer.
//
// The "Reply" prompt (ui_hud_chat_reply, the chat_init action on Q) is the game's own C++
// (the player's chat hints): it shows while the dialogue system holds a person's chat
// request to the player (a dialogue decision with RequestChat, such as a remark on the player's
// looks) or a chat the player accepted. Both are asked for through (the pending request,
// dialogue module + 160) and (the accepted chat, + 144); each answers an object whose
// slot 11 is the person's entity id. Both are hooked: a request of a server NPC players may not
// chat with is no request, so neither its Reply nor its Q shows. Engine thread only.
#pragma once

#include <cstdint>

namespace kcds::runtime::interactions::talk {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* can_talk = nullptr;       // C_Human::CanTalk
  void* can_chat = nullptr;       // C_Human::CanChat
  void* chat_request = nullptr;   // the pending chat request
  void* accepted_chat = nullptr;  // the accepted chat
  // Whether an entity is a server NPC's body here, and whether players may not chat with it
  // (npcChat off, or Npc.interactable false).
  bool (*npc_entity_closed)(std::uint32_t entity) = nullptr;
  // Whether a soul is a server NPC's body here, or another player's replica.
  bool (*npc_soul)(const void* soul) = nullptr;
  // Whether a server NPC's body is one players cannot talk to (Npc.interactable false): none of
  // its prompts show, whatever the server's rules.
  bool (*npc_closed)(const void* soul) = nullptr;
  bool (*replica_soul)(const void* soul) = nullptr;
};

// False when a native is missing or a hook fails; the prompts are then the game's own.
bool install(const Natives& natives, Logger logger);

}  // namespace kcds::runtime::interactions::talk
