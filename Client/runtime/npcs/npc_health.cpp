#include "npc_health.hpp"

#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace kcds::runtime::npcs::health {
namespace {

// x64 has one calling convention (no __fastcall: the Linux unit test builds this file too).
using SetState = void (*)(void* soul, unsigned state, float value);
using DealDamage = void (*)(void* damage, float stamina, float health, void* attacker, char flag,
                            void* type);
using DamageTypeInit = void* (*)(void* type);
using DamageTypeFree = void (*)(void* type);

constexpr std::size_t kSoulHealth = 1800;
constexpr std::size_t kSoulDamage = 240;
constexpr std::size_t kDealDamage = 27;
constexpr unsigned kHealthState = 0;
// A difference smaller than this is not shown again.
constexpr float kHealthTolerance = 0.01f;
// At most this many hits wait for the engine thread.
constexpr std::size_t kMaxQueuedHits = 64;

Natives natives;
std::mutex bodies_mutex;
std::unordered_map<std::uint32_t, Body> by_entity;
std::unordered_map<const void*, Body> by_soul;
std::unordered_map<std::uint32_t, float> damage_shares;
// The NPCs the server made untargetable (Npc.targetable false).
std::unordered_set<std::uint32_t> untargetable;
// The NPCs players cannot fight (Npc.attackable false).
std::unordered_set<std::uint32_t> unattackable;
// The NPCs players cannot talk to (Npc.interactable false).
std::unordered_set<std::uint32_t> closed;
// The NPCs the HUD does not name when a player looks at them (Npc.nameShown false).
std::unordered_set<std::uint32_t> nameless;
std::unordered_set<std::uint32_t> closed_carcasses;
std::vector<kcd2s::integration::NpcHit> queued_hits;
std::vector<kcd2s::integration::NpcBlow> queued_blows;
thread_local bool dealing_lethal_damage = false;

void erase_npc(std::uint32_t npc) {
  for (auto it = by_entity.begin(); it != by_entity.end();) {
    if (it->second.npc == npc) {
      it = by_entity.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = by_soul.begin(); it != by_soul.end();) {
    if (it->second.npc == npc) {
      it = by_soul.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

bool install(const Natives& resolved) {
  natives = resolved;
  return natives.set_state && natives.damage_type_init && natives.damage_type_free;
}

void track(std::uint32_t engine_id, void* soul, const Body& body) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  erase_npc(body.npc);
  by_entity[engine_id] = body;
  if (soul) {
    by_soul[soul] = body;
  }
}

void forget(std::uint32_t npc) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  erase_npc(npc);
  damage_shares.erase(npc);
  untargetable.erase(npc);
  nameless.erase(npc);
  closed_carcasses.erase(npc);
}

void set_carcass_closed(std::uint32_t npc, bool closed) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  if (closed) {
    closed_carcasses.insert(npc);
  } else {
    closed_carcasses.erase(npc);
  }
}

bool carcass_closed(std::uint32_t npc) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  return closed_carcasses.count(npc) != 0;
}

void set_attackable(std::uint32_t npc, bool attackable) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  if (attackable) {
    unattackable.erase(npc);
  } else {
    unattackable.insert(npc);
  }
}

bool attackable(std::uint32_t npc) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  return unattackable.count(npc) == 0;
}

void set_interactable(std::uint32_t npc, bool interactable) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  if (interactable) {
    closed.erase(npc);
  } else {
    closed.insert(npc);
  }
}

bool interactable(std::uint32_t npc) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  return closed.count(npc) == 0;
}

void set_name_shown(std::uint32_t npc, bool shown) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  if (shown) {
    nameless.erase(npc);
  } else {
    nameless.insert(npc);
  }
}

bool name_shown(std::uint32_t npc) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  return nameless.count(npc) == 0;
}

bool name_shown_of_entity(std::uint32_t engine_id) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  const auto body = by_entity.find(engine_id);
  return body == by_entity.end() || nameless.count(body->second.npc) == 0;
}

void set_targetable(std::uint32_t npc, bool targetable) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  if (targetable) {
    untargetable.erase(npc);
  } else {
    untargetable.insert(npc);
  }
}

bool targetable(std::uint32_t npc) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  return untargetable.count(npc) == 0;
}

void set_damage_share(std::uint32_t npc, float share) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  if (share == kcd2s::integration::kNpcDamageGame) {
    damage_shares.erase(npc);
    return;
  }
  damage_shares[npc] = share;
}

float damage_share(std::uint32_t npc) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  const auto found = damage_shares.find(npc);
  return found != damage_shares.end() ? found->second : kcd2s::integration::kNpcDamageGame;
}

void clear() {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  by_entity.clear();
  by_soul.clear();
  damage_shares.clear();
  untargetable.clear();
  nameless.clear();
  queued_hits.clear();
  queued_blows.clear();
}

std::optional<Body> body_of_entity(std::uint32_t engine_id) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  auto it = by_entity.find(engine_id);
  if (it == by_entity.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<Body> body_of_soul(const void* soul) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  auto it = by_soul.find(soul);
  if (it == by_soul.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<kcd2s::integration::NpcHit> hit_to_report(const Body& body, Source source,
                                                        std::uint32_t attacker, float amount,
                                                        std::uint8_t reason, std::int32_t subpart) {
  if (!(amount > 0.0f) || !(amount <= kcd2s::integration::kMaxDamage) ||
      reason >= kcd2s::integration::kDamageReasons) {
    return std::nullopt;
  }
  kcd2s::integration::NpcHit hit;
  hit.npc = body.npc;
  hit.amount = amount;
  hit.reason = reason;
  hit.subpart = subpart < -1 ? -1 : subpart;
  switch (source) {
    case Source::LocalPlayer:
      hit.attacker = attacker;
      return hit;
    case Source::Npc:
      if (!body.simulated) {
        return std::nullopt;
      }
      hit.attacker = attacker;
      return hit;
    case Source::World:
      if (!body.simulated) {
        return std::nullopt;
      }
      hit.attacker = 0;
      return hit;
    case Source::RemotePlayer:
      return std::nullopt;
  }
  return std::nullopt;
}

void queue_hit(const kcd2s::integration::NpcHit& hit) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  if (queued_hits.size() < kMaxQueuedHits) {
    queued_hits.push_back(hit);
  }
}

std::vector<kcd2s::integration::NpcHit> take_hits() {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  return std::exchange(queued_hits, {});
}

void queue_blow(const kcd2s::integration::NpcBlow& blow) {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  if (queued_blows.size() < kMaxQueuedHits) {
    queued_blows.push_back(blow);
  }
}

std::vector<kcd2s::integration::NpcBlow> take_blows() {
  std::lock_guard<std::mutex> lock(bodies_mutex);
  return std::exchange(queued_blows, {});
}

float health_of(const void* soul) {
  float health = 0;
  std::memcpy(&health, static_cast<const std::uint8_t*>(soul) + kSoulHealth, sizeof(health));
  return health;
}

void show_health(void* soul, float health) {
  if (!soul || !natives.set_state || !(health > 0.0f)) {
    return;
  }
  if (std::fabs(health_of(soul) - health) < kHealthTolerance) {
    return;
  }
  reinterpret_cast<SetState>(natives.set_state)(soul, kHealthState, health);
}

void kill(void* soul) {
  if (!soul || !natives.damage_type_init || !natives.damage_type_free) {
    return;
  }
  void* damage = static_cast<std::uint8_t*>(soul) + kSoulDamage;
  alignas(16) std::uint8_t type[64] = {};
  reinterpret_cast<DamageTypeInit>(natives.damage_type_init)(type);
  const float lethal = health_of(soul) + 1.0f;
  auto deal = reinterpret_cast<DealDamage>((*reinterpret_cast<void***>(damage))[kDealDamage]);
  dealing_lethal_damage = true;
  deal(damage, 0.0f, lethal, nullptr, 0, type);
  dealing_lethal_damage = false;
  reinterpret_cast<DamageTypeFree>(natives.damage_type_free)(type);
}

bool killing() {
  return dealing_lethal_damage;
}

}  // namespace kcds::runtime::npcs::health
