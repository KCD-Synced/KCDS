// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "held_harm.hpp"

#include "../core/log.hpp"
#include "../knockout/knockout.hpp"
#include "../npcs/npc_health.hpp"
#include "../stats/player_stats.hpp"
#include "combat_identity.hpp"
#include "player_damage.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

namespace {

// Held harm by request: what the game would have taken, and when it was held.
struct HeldHarm {
  integration::PlayerHarm harm;
  std::uint64_t held_at = 0;
};
// A ruling waits at most this long; at most this many wait at once (more are taken at once).
constexpr std::uint64_t kHarmRulingMs = 1500;
constexpr std::size_t kMaxHeldHarm = 32;
// Requests of held harm carry the top bit, apart from held blows' (held_hits.cpp).
constexpr std::uint32_t kHarmRequestBit = 0x80000000u;
// The game's damage reasons never held: ticks (starvation, poison, bleeding).
constexpr std::uint8_t kStarvation = 3;
constexpr std::uint8_t kPoisoning = 8;
constexpr std::uint8_t kBleeding = 9;

// One damage call on the local player being held: the packet gives the game only a base value
// (a fall comes as 2 with the scripted reason), and the game works out the health it takes and
// the reason inside the call. Its health write is kept back here (block_harm_write) and becomes
// the held harm once the call ends.
struct Capture {
  void* soul = nullptr;
  float before = 0.0f;
  float lowest = 0.0f;
  bool active = false;
};
thread_local Capture capture;
// Harm kept back this frame, asked about at the frame's tick: a hit's health effect comes just
// before the game queues the hit, and only the hit names its attacker (harm_hit_queued).
std::vector<integration::PlayerHarm> pending;
// take() applies a ruled amount; its own health change is never held again.
thread_local bool taking = false;

std::atomic<bool> harm_held{false};
std::unordered_map<std::uint32_t, HeldHarm> held;
std::uint32_t next_request = 1;

void* local_soul() {
  auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  return actor ? *reinterpret_cast<void**>(actor + 1640) : nullptr;
}

// Takes `amount` of the local player's health the way addStat does (the game's own lethal damage
// at 0, a knock-out in a server world) and reports it as the harm it was.
void take(const integration::PlayerHarm& harm, float amount) {
  void* soul = local_soul();
  if (!soul || !(amount > 0.0f) || kcds::runtime::knockout::knocked_out()) {
    return;
  }
  const float before = *reinterpret_cast<float*>(static_cast<std::uint8_t*>(soul) + 1800);
  integration::PlayerDamage damage;
  damage.amount = std::min(amount, before);
  damage.reason = harm.reason;
  damage.subpart = harm.subpart;
  {
    std::lock_guard<std::mutex> lock(player_damage_mutex);
    last_damage_ms = GetTickCount64();
    last_damage_sequence = ++damage_receipt_sequence;
    if (damage.amount > 0.0f && !player_damage.push_ready(damage, last_damage_ms)) {
      log("HARM damage report queue full");
    }
  }
  integration::StatRequest request;
  request.stat = 0;
  request.value = -amount;
  request.relative = true;
  taking = true;
  kcds::runtime::stats::apply(soul, {request}, false, nullptr);
  taking = false;
}

// Asks the server about harm the game wanted to take; false when it could not be sent.
bool send_held(integration::PlayerHarm harm) {
  harm.request = kHarmRequestBit | (next_request++ & ~kHarmRequestBit);
  if (next_request & kHarmRequestBit) {
    next_request = 1;
  }
  bool sent = false;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    sent = client && client->peer().send_player_harm(harm);
  }
  if (!sent) {
    return false;
  }
  held[harm.request] = {harm, GetTickCount64()};
  log("HARM held request=%u reason=%u health=%.2f attacker=%u", harm.request, unsigned(harm.reason),
      harm.amount, harm.attacker);
  return true;
}

}  // namespace

void set_harm_rules(bool rules) {
  if (harm_held.exchange(rules) != rules) {
    log("HARM rulings %s", rules ? "on" : "off");
  }
}

bool begin_harm_hold(std::uint32_t reason, void* attacker) {
  void* soul = local_soul();
  if (!harm_held.load() || !soul || taking || capture.active ||
      held.size() + pending.size() >= kMaxHeldHarm) {
    return false;
  }
  if (reason == kStarvation || reason == kPoisoning || reason == kBleeding) {
    return false;
  }
  // Another player's blow is held as a blow (held_hits.cpp), never as harm.
  if (attacker) {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    if (remote_combat_souls.count(attacker)) {
      return false;
    }
  }
  const float health = *reinterpret_cast<float*>(static_cast<std::uint8_t*>(soul) + 1800);
  capture = {soul, health, health, true};
  return true;
}

bool harm_hit_queued(std::uint32_t source_entity) {
  if (!harm_held.load()) {
    return false;
  }
  if (!pending.empty() && pending.back().attacker == 0) {
    if (const auto body = kcds::runtime::npcs::health::body_of_entity(source_entity)) {
      pending.back().attacker = body->npc;
    }
  }
  return true;
}

bool block_harm_write(void* soul, float current, float value) {
  if (!capture.active || soul != capture.soul || !(value < current)) {
    return false;
  }
  capture.lowest = std::min(capture.lowest, value);
  return true;
}

void end_harm_hold(std::uint32_t reason, std::int32_t subpart, void* attacker) {
  if (!capture.active) {
    return;
  }
  const Capture done = capture;
  capture = {};
  const float amount = done.before - done.lowest;
  if (!(amount > 0.0f)) {
    return;
  }
  integration::PlayerHarm harm;
  harm.amount = std::min(amount, integration::kMaxDamage);
  harm.reason = reason < integration::kDamageReasons ? std::uint8_t(reason) : std::uint8_t(0);
  harm.subpart = subpart < -1 ? -1 : subpart;
  if (attacker) {
    if (const auto body = kcds::runtime::npcs::health::body_of_soul(attacker)) {
      harm.attacker = body->npc;
    }
  }
  // A tick the game only named in the call is taken now.
  if (reason == kStarvation || reason == kPoisoning || reason == kBleeding) {
    take(harm, amount);
    return;
  }
  pending.push_back(harm);
}

void rule_held_harm(std::vector<integration::HitVerdict>& verdicts) {
  for (auto it = verdicts.begin(); it != verdicts.end();) {
    const auto found = held.find(it->request);
    if ((it->request & kHarmRequestBit) == 0 || found == held.end()) {
      ++it;
      continue;
    }
    const integration::PlayerHarm harm = found->second.harm;
    held.erase(found);
    float amount = 0;
    if (it->ruling == integration::HitRuling::Native) {
      amount = harm.amount;
    } else if (it->ruling == integration::HitRuling::Set) {
      amount = it->value;
    } else if (it->ruling == integration::HitRuling::Scaled) {
      amount = harm.amount * it->value;
    }
    log("HARM ruled request=%u ruling=%u health=%.2f", harm.request, unsigned(it->ruling), amount);
    take(harm, amount);
    it = verdicts.erase(it);
  }
}

void tick_held_harm() {
  // This frame's harm goes to the server; what cannot be asked about is taken now.
  for (const integration::PlayerHarm& harm : pending) {
    if (!send_held(harm)) {
      log("HARM taken at once health=%.2f reason=%u", harm.amount, unsigned(harm.reason));
      take(harm, harm.amount);
    }
  }
  pending.clear();
  const std::uint64_t now = GetTickCount64();
  for (auto it = held.begin(); it != held.end();) {
    if (now - it->second.held_at < kHarmRulingMs) {
      ++it;
      continue;
    }
    const integration::PlayerHarm harm = it->second.harm;
    it = held.erase(it);
    log("HARM no ruling for request=%u: taken as the game would have", harm.request);
    take(harm, harm.amount);
  }
}

void discard_held_harm(const char* reason) {
  if (!held.empty() || !pending.empty()) {
    log("HARM %zu held dropped: %s", held.size() + pending.size(), reason);
  }
  held.clear();
  pending.clear();
}

}  // namespace kcds::game_client
