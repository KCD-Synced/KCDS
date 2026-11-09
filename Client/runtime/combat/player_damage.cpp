// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "player_damage.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../knockout/knockout.hpp"
#include "../npcs/npc_health.hpp"
#include "../replicas/remote.hpp"
#include "../riding/trample.hpp"
#include "damage_reports.hpp"
#include "held_harm.hpp"

#include <utility>
#include <cstring>
#include <mutex>
#include <vector>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// The health the local player's game took off them, waiting for the network tick.
std::mutex player_damage_mutex;
kcds::runtime::combat::DamageReports player_damage;
// The last tick of bleeding, poisoning or hunger (not reported by itself) and when it came, and
// when the last damage was queued: a knock-out right after such a tick reports it as the blow
// that killed (GetTickCount64 ms; player_damage_mutex).
kcd2s::integration::PlayerDamage last_quiet_tick;
ULONGLONG last_quiet_tick_ms = 0;
ULONGLONG last_damage_ms = 0;
std::uint64_t damage_receipt_sequence = 0;
std::uint64_t last_quiet_tick_sequence = 0;
std::uint64_t last_damage_sequence = 0;

void discard_player_damage(const char* reason) {
  std::size_t discarded = 0;
  {
    std::lock_guard<std::mutex> lock(player_damage_mutex);
    discarded = player_damage.size();
    player_damage.clear();
    last_quiet_tick = {};
    last_quiet_tick_ms = 0;
    last_damage_ms = 0;
    last_quiet_tick_sequence = 0;
    last_damage_sequence = 0;
  }
  if (discarded) {
    log("DAMAGE discarded %zu pending reports: %s", discarded, reason);
  }
}

// Positive scripted setters use the native state setter rather than the combat-damage packet.
// Report each measured reduction at that call's position in the ordered request batch.
void queue_scripted_health_reduction(float amount) {
  if (!(amount > 0.0f) || !(amount <= integration::kMaxDamage)) {
    return;
  }
  integration::PlayerDamage damage;
  damage.amount = amount;
  damage.reason = 5;  // native E_DamageReason::ScriptedHit; no attacker or body part
  std::lock_guard<std::mutex> lock(player_damage_mutex);
  last_damage_ms = GetTickCount64();
  last_damage_sequence = ++damage_receipt_sequence;
  if (!player_damage.push_ready(damage, last_damage_ms)) {
    log("DAMAGE report queue full; scripted reduction not admitted");
  }
}

// The game's damage packet ('s second argument): stamina at + 0, health at + 4, the
// attacker's soul at + 8, the body subpart at + 24 (-1 for none), the reason at + 92
// (E_DamageReason). Bleeding, poisoning and hunger wear health down a tick at a time: not
// reported.
void queue_player_damage(std::uint8_t* soul, float before, const void* packet) {
  constexpr std::uint8_t kStarvation = 3;
  constexpr std::uint8_t kPoisoning = 8;
  constexpr std::uint8_t kBleeding = 9;
  const auto* bytes = static_cast<const std::uint8_t*>(packet);
  const float after = *reinterpret_cast<float*>(soul + 1800);
  std::uint32_t reason = 0;
  std::int32_t subpart = -1;
  void* attacker = nullptr;
  std::memcpy(&reason, bytes + 92, sizeof(reason));
  std::memcpy(&subpart, bytes + 24, sizeof(subpart));
  std::memcpy(&attacker, bytes + 8, sizeof(attacker));
  if (!(before - after > 0.0f) || reason >= kcd2s::integration::kDamageReasons)
    return;
  kcd2s::integration::PlayerDamage damage;
  damage.amount = before - after;
  damage.reason = std::uint8_t(reason);
  damage.subpart = subpart < -1 ? -1 : subpart;
  if (attacker) {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    if (auto it = remote_combat_souls.find(attacker); it != remote_combat_souls.end())
      damage.attacker = std::uint32_t(it->second);
  }
  std::lock_guard<std::mutex> lock(player_damage_mutex);
  const auto receipt_sequence = ++damage_receipt_sequence;
  if (reason == kStarvation || reason == kPoisoning || reason == kBleeding) {
    last_quiet_tick = damage;
    last_quiet_tick_ms = GetTickCount64();
    last_quiet_tick_sequence = receipt_sequence;
    return;
  }
  last_damage_ms = GetTickCount64();
  last_damage_sequence = receipt_sequence;
  if (!player_damage.push_ready(damage, last_damage_ms)) {
    log("DAMAGE report queue full; native packet report not admitted");
  }
}

// A knock-out came: the bleeding, poisoning or hunger tick just before it, with nothing reported
// since, is the blow that killed and is reported.
void queue_killing_tick() {
  std::lock_guard<std::mutex> lock(player_damage_mutex);
  const ULONGLONG now = GetTickCount64();
  if (!kcds::runtime::combat::quiet_damage_eligible(
          now, last_quiet_tick_ms, last_quiet_tick_sequence, last_damage_sequence)) {
    return;
  }
  if (!player_damage.push_ready(last_quiet_tick, last_quiet_tick_ms)) {
    log("DAMAGE report queue full; killing condition report not admitted");
  }
  last_quiet_tick_ms = 0;
}

// The player whose replica dealt a hit (the hit's source entity id at + 0), 0 for anyone else.
std::uint32_t hit_attacker(const std::uint8_t* hit) {
  const auto source = *reinterpret_cast<const unsigned*>(hit);
  std::lock_guard<std::mutex> lock(combat_identity_mutex);
  for (const auto& [soul, movement] : remote_combat_movements) {
    auto actor = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 504);
    if (!actor || *reinterpret_cast<const unsigned*>(actor + 48) != source) {
      continue;
    }
    auto it = remote_combat_souls.find(soul);
    return it != remote_combat_souls.end() ? std::uint32_t(it->second) : 0;
  }
  return 0;
}

// The body subpart the game priced the latest melee blow on the local player on (its health
// effect's + 48, see hooked_price_blow) and when: the melee hit queued right after it landed there
// Engine thread.
std::int32_t priced_melee_subpart = -1;
ULONGLONG priced_melee_subpart_ms = 0;
std::int32_t priced_npc_subpart = -1;
std::uint32_t priced_npc = 0;
ULONGLONG priced_npc_ms = 0;
constexpr ULONGLONG kPricedSubpartMs = 1000;

// A melee hit on the local player (a HitInfo the game queues,'s input): the game takes
// the health at + 48 when it handles the hit, which does not go through. The attacker
// at + 0 is an entity id: a replica's is a player's. The body part is the one the
// game priced the blow on (+ 52 is the physics part hit, not the body_subpart table's row).
std::optional<integration::PlayerDamage> hit_damage_report(const std::uint8_t* hit) {
  constexpr std::uint8_t kCombat = 1;
  constexpr std::uint8_t kCollision = 4;
  float amount = 0;
  std::memcpy(&amount, hit + 48, sizeof(amount));
  if (!(amount >= 0.0f) || !(amount <= kcd2s::integration::kMaxDamage)) {
    return std::nullopt;
  }
  kcd2s::integration::PlayerDamage damage;
  damage.amount = amount;
  // A horse running into the player (the hit type at + 24, 3 as for kCollisionHit) is a
  // collision.
  constexpr unsigned kCollisionHitType = 3;
  const bool collision = *reinterpret_cast<const unsigned*>(hit + 24) == kCollisionHitType;
  damage.reason = collision ? kCollision : kCombat;
  damage.attacker = hit_attacker(hit);
  // The hit type of a projectile (kProjectileHitType).
  constexpr unsigned kProjectileType = 6;
  const bool projectile = *reinterpret_cast<const unsigned*>(hit + 24) == kProjectileType;
  if (!collision && !projectile && priced_melee_subpart >= 0 &&
      GetTickCount64() - priced_melee_subpart_ms <= kPricedSubpartMs) {
    damage.subpart = priced_melee_subpart;
    priced_melee_subpart = -1;
  }
  return damage;
}

void queue_hit_damage(const std::uint8_t* hit) {
  const auto damage = hit_damage_report(hit);
  if (!damage || !(damage->amount > 0.0f)) {
    return;
  }
  std::lock_guard<std::mutex> lock(player_damage_mutex);
  last_damage_ms = GetTickCount64();
  last_damage_sequence = ++damage_receipt_sequence;
  if (!player_damage.push_ready(*damage, last_damage_ms)) {
    log("DAMAGE report queue full; native hit report not admitted");
  }
}

SetNativeCharacteristic original_set_native_characteristic = nullptr;
thread_local void* authoritative_replica_soul = nullptr;

float __fastcall hooked_set_native_characteristic(void* property, unsigned state, float value,
                                                  void* effect) {
  // A walking body's bump against the local player takes neither stamina (state 1) nor health
  // (state 0) from them (riding/trample.hpp); the states lie at property + 424 on.
  if (property && state <= 1 && bump_just_softened()) {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    void* soul = *reinterpret_cast<void**>(property);
    const float current =
        *reinterpret_cast<float*>(static_cast<std::uint8_t*>(property) + 424 + 4 * state);
    if (actor && soul == *reinterpret_cast<void**>(actor + 1640) && value < current) {
      log("COLLISION bump: the local player keeps state %u at %.2f (asked %.2f)", state, current,
          value);
      return current;
    }
  }
  if (property && state == 0) {
    // property=soul+1376, its first pointer is that soul; health is property+424.
    // The native equal-value path returns the current value without callbacks or death flags.
    auto soul = *reinterpret_cast<void**>(property);
    const float health = *reinterpret_cast<float*>(static_cast<std::uint8_t*>(property) + 424);
    if (block_harm_write(soul, health, value)) {
      return health;
    }
    if (soul != authoritative_replica_soul) {
      core::EntityId replica = core::kInvalidEntity;
      {
        std::lock_guard<std::mutex> lock(combat_identity_mutex);
        const auto found = remote_combat_souls.find(soul);
        if (found != remote_combat_souls.end()) {
          replica = found->second;
        }
      }
      if (replica) {
        const float current = *reinterpret_cast<float*>(static_cast<std::uint8_t*>(property) + 424);
        if (current != value) {
          unsigned ignored = 0;
          {
            std::lock_guard<std::mutex> lock(combat_identity_mutex);
            auto& writes = ignored_health_writes[replica];
            ++writes.since_log;
            const ULONGLONG now = GetTickCount64();
            if (now >= writes.next_log) {
              writes.next_log = now + 1000;
              ignored = writes.since_log;
              writes.since_log = 0;
            }
          }
          if (ignored) {
            log("COMBAT replica health write ignored player=%u current=%.5f requested=%.5f "
                "(%u since the previous line)",
                replica, current, value, ignored);
          }
        }
        return current;
      }
    }
  }
  return original_set_native_characteristic(property, state, value, effect);
}

// The game's damage packet on a body of a server NPC (the packet's layout is
// queue_player_damage's): as a blow on one (take_npc_hit_health), the body takes none of the
// health and the striker's own client, or the NPC's simulator, reports it.
// Where the blow priced just now on this NPC landed, once; -1 for none.
std::int32_t take_priced_npc_subpart(std::uint32_t npc) {
  if (priced_npc != npc || priced_npc_subpart < 0 ||
      GetTickCount64() - priced_npc_ms > kPricedSubpartMs) {
    return -1;
  }
  return std::exchange(priced_npc_subpart, -1);
}

void take_npc_damage_health(const kcds::runtime::npcs::health::Body& body, std::uint8_t* packet,
                            const std::uint8_t* local_actor) {
  namespace npc_health = kcds::runtime::npcs::health;
  float amount = 0;
  std::uint32_t reason = 0;
  std::int32_t subpart = -1;
  void* attacker_soul = nullptr;
  std::memcpy(&amount, packet + 4, sizeof(amount));
  std::memcpy(&attacker_soul, packet + 8, sizeof(attacker_soul));
  std::memcpy(&subpart, packet + 24, sizeof(subpart));
  std::memcpy(&reason, packet + 92, sizeof(reason));
  // The packet names no subpart for an animal: where the blow priced just now landed.
  if (subpart < 0) {
    subpart = take_priced_npc_subpart(body.npc);
  }
  auto source = npc_health::Source::World;
  std::uint32_t attacker = 0;
  bool remote = false;
  if (attacker_soul) {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    remote = remote_combat_souls.count(attacker_soul) != 0;
  }
  if (attacker_soul && local_actor &&
      attacker_soul == *reinterpret_cast<void* const*>(local_actor + 1640)) {
    source = npc_health::Source::LocalPlayer;
    attacker = local_network_id.load();
  } else if (const auto striker = npc_health::body_of_soul(attacker_soul)) {
    source = npc_health::Source::Npc;
    attacker = striker->npc;
  } else if (remote) {
    source = npc_health::Source::RemotePlayer;
  }
  const auto report = npc_health::hit_to_report(body, source, attacker, amount,
                                                std::uint8_t(std::min(reason, 255u)), subpart);
  if (report) {
    npc_health::queue_hit(*report);
  }
  const float none = 0;
  std::memcpy(packet + 4, &none, sizeof(none));
  if (amount > 0.0f) {
    log("NPC damage npc=%u kind=%d reason=%u health=%.2f reported=%u", body.npc, int(source),
        reason, amount, unsigned(report.has_value()));
  }
}

void __fastcall hooked_combat_damage(void* self, const void* packet) {
  auto soul = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(self) + 120);
  auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  if (packet && !kcds::runtime::npcs::health::killing()) {
    if (const auto body = kcds::runtime::npcs::health::body_of_soul(soul)) {
      take_npc_damage_health(*body, static_cast<std::uint8_t*>(const_cast<void*>(packet)), actor);
    }
  }
  // While the local player lies knocked out their body takes no damage: on a KCDS server players
  // are only knocked out, and the server gets them up again.
  if (packet && actor && soul == *reinterpret_cast<void**>(actor + 1640) &&
      (kcds::runtime::knockout::knocked_out() ||
       kcds::runtime::knockout::creature_unconscious(soul))) {
    float amount = 0;
    std::memcpy(&amount, static_cast<const std::uint8_t*>(packet) + 4, sizeof(amount));
    log("KNOCKOUT damage on the lying local player dropped health=%.2f", amount);
    return;
  }
  // Harm a resource rules on (playerHarm) waits for the server's ruling (held_harm.cpp): the call
  // runs, its health write is kept back.
  if (packet && actor && soul == *reinterpret_cast<void**>(actor + 1640)) {
    const auto* bytes = static_cast<const std::uint8_t*>(packet);
    std::uint32_t reason = 0;
    std::int32_t subpart = -1;
    void* attacker = nullptr;
    std::memcpy(&reason, bytes + 92, sizeof(reason));
    std::memcpy(&attacker, bytes + 8, sizeof(attacker));
    if (begin_harm_hold(reason, attacker)) {
      original_combat_damage(self, packet);
      std::memcpy(&reason, bytes + 92, sizeof(reason));
      std::memcpy(&subpart, bytes + 24, sizeof(subpart));
      std::memcpy(&attacker, bytes + 8, sizeof(attacker));
      end_harm_hold(reason, subpart, attacker);
      return;
    }
  }
  bool tracked = actor && soul == *reinterpret_cast<void**>(actor + 1640);
  core::EntityId network_id = core::kInvalidEntity;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    if (auto it = remote_combat_souls.find(soul); it != remote_combat_souls.end()) {
      tracked = true;
      network_id = it->second;
    }
  }
  if (network_id) {
    // would create local RPG buffs and environmental hit/death events for a replica.
    // Its owner evaluates that damage; this client's replica follows owner state and reactions.
    log("COMBAT scripted damage ignored for replica player=%u", network_id);
    return;
  }
  const auto before = tracked ? *reinterpret_cast<float*>(soul + 1800) : 0.0f;
  const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
  original_combat_damage(self, packet);
  // The local player's own damage goes to the server: the health it took, the
  // attacker (a replica's soul), the reason (packet + 92) and the body subpart (packet + 24).
  if (actor && soul == *reinterpret_cast<void**>(actor + 1640))
    queue_player_damage(soul, before, packet);
  if (tracked)
    log("COMBAT native scripted damage soul=%p network=%u health_cache=%.5f->%.5f caller=%p "
        "original-called=1",
        soul, network_id, before, *reinterpret_cast<float*>(soul + 1800),
        reinterpret_cast<void*>(caller));
}

// Health the local player's game took off them, to the server.
void send_player_damage() {
  std::vector<kcds::runtime::combat::DamageReports::Report> damages;
  {
    std::lock_guard<std::mutex> lock(player_damage_mutex);
    damages = player_damage.take_ready();
  }
  for (const auto& report : damages) {
    const auto& damage = report.damage;
    log("DAMAGE local %.2f reason=%u subpart=%d attacker=%u", damage.amount,
        unsigned(damage.reason), damage.subpart, damage.attacker);
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_player_damage(damage);
  }
}

}  // namespace kcds::game_client
