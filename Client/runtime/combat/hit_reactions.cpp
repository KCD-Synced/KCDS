#include "hit_reactions.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../riding/rider_diagnostics.hpp"
#include "../riding/trample.hpp"

#include <cstring>

namespace kcds::game_client {

// diagnostics: the actor's GameRules hit reaction for a collision
// hit, its reaction query and its animation reaction, to find why a
// trampled replica does not fall on the rider's client.
ActorHitReaction original_actor_hit_reaction = nullptr;
HitReactionQuery original_hit_reaction_query = nullptr;
AnimationReaction original_animation_reaction = nullptr;
thread_local bool collision_reaction = false;

// The game's reaction to a collision hit without a source (the body knocked against something) on
// the local body, at most once in kSourcelessCollisionGapMs. A player revived where they had hung
// on a gallows got one every 30 to 100 ms without end, each starting the hit reaction again, and
// lay convulsing on the ground, unable to get up. Real falls and trampling (which
// has a source) keep their reaction.
constexpr ULONGLONG kSourcelessCollisionGapMs = 1500;
ULONGLONG last_sourceless_collision = 0;
bool sourceless_collisions_held = false;

void* __fastcall hooked_hit_reaction_query(void* reactions, const void* hit, int* kind) {
  void* result = original_hit_reaction_query(reactions, hit, kind);
  if (collision_reaction) {
    log("COMBAT TRAMPLE reaction query result=%p kind=%d", result, kind ? *kind : -1);
  }
  return result;
}

char __fastcall hooked_animation_reaction(void* reactions, void* a2, void* a3, void* a4) {
  const char result = original_animation_reaction(reactions, a2, a3, a4);
  if (collision_reaction) {
    log("COMBAT TRAMPLE animation reaction result=%u", unsigned(result));
  }
  return result;
}

// The hit reaction ends by telling the hit's source (-> on the source
// actor's +576 listeners) how the target reacted. For a trample on a replica the source is the
// rider's own horse, and that report left the rider standing on the horse (with the rider's
// HitInfo reaching the replica); the owner's client reports it to its horse replica anyway.
// A collision reaction on a player replica is running (engine thread).
thread_local bool replica_collision_reaction = false;

bool player_replica(void* actor) {
  auto soul = actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 1640) : nullptr;
  std::lock_guard<std::mutex> lock(combat_identity_mutex);
  return soul && remote_combat_souls.count(soul) != 0;
}

SourceHitReport original_source_hit_report = nullptr;

std::int64_t __fastcall hooked_source_hit_report(void* listeners, const void* result,
                                                 const void* hit) {
  if (replica_collision_reaction) {
    log("COMBAT TRAMPLE replica reaction report to the horse skipped");
    return 0;
  }
  return original_source_hit_report(listeners, result, hit);
}

// The hit reaction database query (S_HitReactionQueryData at its second argument).
// Its +48 is whether the target is the player (filled); the trample
// fall of a player is a player reaction, so the replica's query found nothing (live dumps: the
// owner's and the replica's queries differ only there) and the rider never saw the fall. A
// replica's collision reaction is queried again as the player's.
ReactionDatabaseQuery original_reaction_database_query = nullptr;
constexpr std::size_t kReactionQuerySize = 0x78;
constexpr std::size_t kReactionQueryPlayer = 48;

void* __fastcall hooked_reaction_database_query(void* reactions, const void* query) {
  void* result = original_reaction_database_query(reactions, query);
  if (!result && replica_collision_reaction && query) {
    alignas(16) std::uint8_t as_player[kReactionQuerySize];
    std::memcpy(as_player, query, sizeof(as_player));
    as_player[kReactionQueryPlayer] = 1;
    result = original_reaction_database_query(reactions, as_player);
    log("COMBAT TRAMPLE replica reaction queried as the player's result=%p", result);
  }
  if (collision_reaction && query) {
    const auto* words = static_cast<const std::uint32_t*>(query);
    char line[512];
    int used = 0;
    for (unsigned i = 0; i < 32 && used < int(sizeof(line)) - 10; ++i) {
      used += sprintf_s(line + used, sizeof(line) - used, "%08x ", words[i]);
    }
    log("COMBAT TRAMPLE reaction db query result=%p data=%s", result, line);
  }
  return result;
}

char __fastcall hooked_actor_hit_reaction(void* reactions, const unsigned* hit) {
  const bool collision = hit && hit[6] == kCollisionHit;
  auto target = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(reactions) + 40);
  // A trampled player replica gets no reaction of its own: it falls with the ragdoll profile and
  // its owner's ragdoll pose (tick_trample_ragdoll). The game's fall reaction (fragment HitDeath,
  // hasRagdoll) started the game's ragdoll action on the replica, which nothing on this client
  // ends: after a second trample the replica kept a ragdoll body while it stood, so the rider's
  // weapon passed through it and dealt no damage.
  if (collision && player_replica(target)) {
    log("COMBAT TRAMPLE replica reaction skipped target=%u source=%u", hit[10], hit[0]);
    return 0;
  }
  if (collision && hit[0] == 0 && target == local_combat_actor.load()) {
    const ULONGLONG now = GetTickCount64();
    if (now - last_sourceless_collision < kSourcelessCollisionGapMs) {
      if (!sourceless_collisions_held) {
        log("COMBAT collision reactions without a source held on the local body");
        sourceless_collisions_held = true;
      }
      return 0;
    }
    last_sourceless_collision = now;
    sourceless_collisions_held = false;
  }
  collision_reaction = collision;
  replica_collision_reaction = false;
  const char result = original_actor_hit_reaction(reactions, hit);
  replica_collision_reaction = false;
  collision_reaction = false;
  if (collision) {
    auto actor = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(reactions) + 40);
    log("COMBAT TRAMPLE actor reaction target=%u source=%u result=%u local=%u", hit[10], hit[0],
        unsigned(result), unsigned(actor == local_combat_actor.load()));
  }
  return result;
}

}  // namespace kcds::game_client
