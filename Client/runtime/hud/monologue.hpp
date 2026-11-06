// The local character's monologue: Hud.setInnerVoice.
//
// The game voices and captions the player's own remarks (horse commands, comments on screens and
// photo mode) in Henry's voice whoever plays, so multiplayer suppresses them by default. Every
// such remark is an AI signal "dialog:monologRequest" to the local player's entity with
// "metarole('<metarole>'),..." parameters, sent (the horse and the UI comment
// table both go through it); the hook drops it while the monologue is disabled and reports one
// line of the metarole's pool, as the dialogue system would have picked one: its localization key
// (monologue_lines.inc, Dev/tools/monologue/build_monologue_lines.py) and its text in the player's
// language (the game's LocalizeLabel, branding::localize). A metarole with no player monologue is
// reported by name with no text. The local player's entity id is the game's own (env +464, its
// vtable +48, then +1072 of the player).
#pragma once

#include <cstdint>
#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::hud::monologue {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
using Logger = void (*)(const char* format, ...);

struct Natives {
  void* send_signal = nullptr;
  void* env = nullptr;
};

bool install(const Natives& natives, Logger logger);

// Whether a dialog of these participants (the game's dialog request, its ids at + 88 to + 96) is
// the local player talking to themselves while the monologue is disabled: their comments on their
// own state (tired, hungry, wet; Localization/dialog/open_world/stavy_hrace_*) start as a dialog
// of the player alone, not as a monologRequest (riding into water).
bool player_alone(const std::uint64_t* first, const std::uint64_t* last);

// { op: "hudMonologue", enabled }; true when the message was this module's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Network thread: reports suppressed lines as { op: "hudMonologueSuppressed", textKey, text } to
// the runtime of session; a new session suppresses the monologue again.
void pump(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::hud::monologue
