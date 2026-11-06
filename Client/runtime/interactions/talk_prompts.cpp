#include "talk_prompts.hpp"

#include <Windows.h>

#include <cstdint>
#include <unordered_map>

#include "../input/interaction_rules.hpp"
#include "MinHook.h"
#include "kcd2s/integration/interaction_rules.hpp"

namespace kcds::runtime::interactions::talk {
namespace {

namespace rules = kcds::runtime::input::interaction_rules;
namespace wire = kcd2s::integration;

// An actor's soul (C_Actor + 1640).
constexpr std::size_t kActorSoul = 1640;

using CanTalk = bool(__fastcall*)(void* human, std::uint32_t user, void* context);
using CanChat = bool(__fastcall*)(void* human, std::uint32_t user);
using ChatRequest = void*(__fastcall*)(void* dialogues);
using AcceptedChat = void*(__fastcall*)();
// A chat request's slot 11: the entity id of the person who asked.
constexpr std::size_t kRequestEntity = 11;
using RequestEntity = std::uint64_t(__fastcall*)(void* request);

Natives natives;
CanTalk original_can_talk = nullptr;
CanChat original_can_chat = nullptr;
ChatRequest original_chat_request = nullptr;
AcceptedChat original_accepted_chat = nullptr;
Logger log_line = nullptr;
// When a held-back Reply was last logged, per person (once a minute each).
std::unordered_map<const void*, std::uint64_t> reply_logged;
constexpr std::uint64_t kReplyLogMs = 60000;

// Logs that a Reply the game would have offered on this person was held back.
void note_reply_held(const void* human, const char* question) {
  const std::uint64_t now = GetTickCount64();
  auto& last = reply_logged[human];
  if (log_line && (last == 0 || now - last >= kReplyLogMs)) {
    last = now;
    log_line("INTERACTIONS reply prompt held back on %p (%s)", human, question);
  }
}

const void* soul_of(const void* human) {
  return human
             ? *reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(human) + kActorSoul)
             : nullptr;
}

// Whether the server allows the prompt on this person: `npc_bit` on a server NPC, playerTalk on
// a player's replica, the game's own answer on anyone else.
bool allowed_on(const void* human, std::uint16_t npc_bit) {
  const void* soul = soul_of(human);
  if (!soul) {
    return true;
  }
  if (natives.npc_soul && natives.npc_soul(soul)) {
    if (natives.npc_closed && natives.npc_closed(soul)) {
      return false;
    }
    return rules::allowed(npc_bit);
  }
  if (natives.replica_soul && natives.replica_soul(soul)) {
    return rules::allowed(wire::kAllowPlayerTalk);
  }
  return true;
}

bool __fastcall hooked_can_talk(void* human, std::uint32_t user, void* context) {
  if (!allowed_on(human, wire::kAllowNpcTalk)) {
    return false;
  }
  return original_can_talk(human, user, context);
}

bool __fastcall hooked_can_chat(void* human, std::uint32_t user) {
  if (!allowed_on(human, wire::kAllowNpcChat)) {
    return false;
  }
  return original_can_chat(human, user);
}

// Whether the chat request (or accepted chat) is one of a server NPC players may not chat with.
bool closed_request(void* request) {
  if (!request || !natives.npc_entity_closed) {
    return false;
  }
  const auto entity = (*reinterpret_cast<RequestEntity**>(request))[kRequestEntity](request);
  return natives.npc_entity_closed(static_cast<std::uint32_t>(entity));
}

// The pending chat request, none when it is a closed server NPC's.
void* __fastcall hooked_chat_request(void* dialogues) {
  void* request = original_chat_request(dialogues);
  if (closed_request(request)) {
    note_reply_held(request, "chat request");
    return nullptr;
  }
  return request;
}

// The accepted chat, none when it is a closed server NPC's.
void* __fastcall hooked_accepted_chat() {
  void* chat = original_accepted_chat();
  if (closed_request(chat)) {
    note_reply_held(chat, "accepted chat");
    return nullptr;
  }
  return chat;
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  const bool talk = hook(natives.can_talk, reinterpret_cast<void*>(hooked_can_talk),
                         reinterpret_cast<void**>(&original_can_talk));
  const bool chat = hook(natives.can_chat, reinterpret_cast<void*>(hooked_can_chat),
                         reinterpret_cast<void**>(&original_can_chat));
  const bool reply = hook(natives.chat_request, reinterpret_cast<void*>(hooked_chat_request),
                          reinterpret_cast<void**>(&original_chat_request)) &&
                     hook(natives.accepted_chat, reinterpret_cast<void*>(hooked_accepted_chat),
                          reinterpret_cast<void**>(&original_accepted_chat));
  if (logger) {
    logger("INTERACTIONS talk prompts %s, chat prompts %s, reply prompts %s",
           talk ? "hooked" : "unavailable", chat ? "hooked" : "unavailable",
           reply ? "hooked" : "unavailable");
  }
  return talk && chat && reply;
}

}  // namespace kcds::runtime::interactions::talk
