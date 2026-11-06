// Players fighting players call no guards.
//
// The game files a crime report ("assault", "aggression", ...) when an NPC sees a fight that has
// the player in it. Its AI script handleStimulusCombat asks GetAllSkirmishParticipants for the
// fight's participants and reports only when ContainsPlayer is set. In a server world every other
// player's body is an NPC to this game, so two players fighting looked like the local player
// assaulting a villager, and guards came.
//
// The hook keeps the game's answer, and clears ContainsPlayer when every participant is a player
// (the local one or a replica): a fight with any NPC in it is reported as before.
#pragma once

#include <cstdint>

namespace kcds::runtime::crime::pvp_crime {

// Whether a participant id (the WUID the game keeps in the participant vector) is a player's.
using IsPlayerId = bool (*)(std::uint64_t id);
using Logger = void (*)(const char* format, ...);

// `participants`: the game's GetAllSkirmishParticipants core.
bool install(void* participants, IsPlayerId is_player_id, Logger logger);

}  // namespace kcds::runtime::crime::pvp_crime
