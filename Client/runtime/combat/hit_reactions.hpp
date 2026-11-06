// The game's hit reactions on collision hits: the diagnostics around them and the
// replica's reaction to a trample, which follows its owner's ragdoll instead.
#pragma once

#include <cstdint>

namespace kcds::game_client {

using ActorHitReaction = char(__fastcall*)(void* reactions, const unsigned* hit);
using HitReactionQuery = void*(__fastcall*)(void* reactions, const void* hit, int* kind);
using AnimationReaction = char(__fastcall*)(void* reactions, void* a2, void* a3, void* a4);
extern ActorHitReaction original_actor_hit_reaction;
extern HitReactionQuery original_hit_reaction_query;
extern AnimationReaction original_animation_reaction;
void* __fastcall hooked_hit_reaction_query(void* reactions, const void* hit, int* kind);
char __fastcall hooked_animation_reaction(void* reactions, void* a2, void* a3, void* a4);
char __fastcall hooked_actor_hit_reaction(void* reactions, const unsigned* hit);

// The hit reaction's report to its source, skipped for a replica's trample reaction.
using SourceHitReport = std::int64_t(__fastcall*)(void* listeners, const void* result,
                                                  const void* hit);
extern SourceHitReport original_source_hit_report;
std::int64_t __fastcall hooked_source_hit_report(void* listeners, const void* result,
                                                 const void* hit);

// The hit reaction database query, asked again as the player's for a replica's trample.
using ReactionDatabaseQuery = void*(__fastcall*)(void* reactions, const void* query);
extern ReactionDatabaseQuery original_reaction_database_query;
void* __fastcall hooked_reaction_database_query(void* reactions, const void* query);

}  // namespace kcds::game_client
