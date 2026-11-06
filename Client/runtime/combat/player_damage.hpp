// The local player's damage for the server: the reports waiting for the network tick,
// the killing tick, a melee hit's report, and the game's damage paths into them. Also the replica
// health guard (only the owner's committed health is written to a replica's soul) and the health
// a body of a server NPC takes. discard_player_damage, queue_killing_tick, hit_attacker,
// queue_hit_damage and queue_scripted_health_reduction are declared in core/client_internal.hpp.
#pragma once

#include <Windows.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "damage_reports.hpp"
#include "kcd2s/core/ids.hpp"
#include "kcd2s/integration/combat.hpp"

namespace kcds::game_client {

extern std::mutex player_damage_mutex;
extern kcds::runtime::combat::DamageReports player_damage;
// When the last damage was queued and the receipt order of the reports (player_damage_mutex).
extern ULONGLONG last_damage_ms;
extern std::uint64_t damage_receipt_sequence;
extern std::uint64_t last_damage_sequence;
void queue_player_damage(std::uint8_t* soul, float before, const void* packet);
// The body subpart the game priced the latest melee blow on the local player on, and when.
extern std::int32_t priced_melee_subpart;
// Where the last blow priced on a server NPC landed, and which NPC: the game's
// damage packet on an animal names no subpart.
extern std::int32_t priced_npc_subpart;
extern std::uint32_t priced_npc;
extern ULONGLONG priced_npc_ms;
std::int32_t take_priced_npc_subpart(std::uint32_t npc);
extern ULONGLONG priced_melee_subpart_ms;
std::optional<kcd2s::integration::PlayerDamage> hit_damage_report(const std::uint8_t* hit);

// The soul's characteristic setter (hooked: a replica's health is written only as its owner's).
using SetNativeCharacteristic = float(__fastcall*)(void*, unsigned, float, void*);
extern SetNativeCharacteristic original_set_native_characteristic;
extern thread_local void* authoritative_replica_soul;
// While it lives, health writes to this replica soul are its owner's committed health.
class ReplicaVitalsWrite {
 public:
  explicit ReplicaVitalsWrite(void* soul) : previous_(authoritative_replica_soul) {
    authoritative_replica_soul = soul;
  }

  ~ReplicaVitalsWrite() {
    authoritative_replica_soul = previous_;
  }

 private:
  void* previous_;
};
// A replica's own game keeps lowering a bleeding replica's health every frame, and every write is
// ignored (its owner's health is kept): one log line per replica and second says so, with how many
// writes were ignored since the previous line. Guarded by combat_identity_mutex.
struct IgnoredHealthWrites {
  ULONGLONG next_log = 0;
  unsigned since_log = 0;
};
extern std::unordered_map<kcd2s::core::EntityId, IgnoredHealthWrites> ignored_health_writes;
float __fastcall hooked_set_native_characteristic(void* property, unsigned state, float value,
                                                  void* effect);
// The game's scripted damage (hooked: reported, kept off replicas and server NPCs' bodies).
void __fastcall hooked_combat_damage(void* self, const void* packet);

}  // namespace kcds::game_client
