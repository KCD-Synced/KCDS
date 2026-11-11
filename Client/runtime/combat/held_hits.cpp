// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "held_harm.hpp"
#include "held_hits.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../input/input_mode.hpp"
#include "../knockout/knockout.hpp"
#include "../npcs/npc_blows.hpp"
#include "../npcs/npc_health.hpp"
#include "../stats/player_stats.hpp"
#include "../replicas/remote.hpp"
#include "../riding/trample.hpp"
#include "combat_identity.hpp"
#include "melee_contacts.hpp"
#include "player_damage.hpp"
#include "priced_hits.hpp"

#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// Blows from other players on the local player, held before the game works them out until the
// server rules on them: the relayed contact, by request. One with no verdict after
// kHitRulingMs is dropped. Engine thread.
struct HeldContact {
  kcd2s::integration::MeleeEvent impact;
  std::uint64_t held_at = 0;
  std::uint64_t life = 0;
};
constexpr std::uint64_t kHitRulingMs = 2000;
constexpr std::size_t kMaxHeldContacts = 64;
std::unordered_map<std::uint32_t, HeldContact> held_contacts;
std::uint32_t next_hit_request = 1;
std::uint64_t local_combat_life = 1;
void* pending_combat_soul = nullptr;
kcds::runtime::knockout::IsUnconscious native_soul_is_unconscious = nullptr;
RequestReplicaWake native_request_replica_wake = nullptr;
void request_replica_native_wake(void* actor, void* state) {
  void* previous = input_mode_actor;
  __try {
    input_mode_actor = actor;
    native_request_replica_wake(state);
  } __finally {
    input_mode_actor = previous;
  }
}
std::uint32_t reserve_hit_request();

// Holds a relayed contact on the local player and asks the server; false when too many wait.
bool hold_contact(const kcd2s::integration::MeleeEvent& impact) {
  if (held_contacts.size() >= kMaxHeldContacts) {
    return false;
  }
  kcd2s::integration::PlayerHitReport report;
  report.request = reserve_hit_request();
  if (!report.request) {
    return false;
  }
  report.attacker = std::uint32_t(impact.source);
  // The contact's hit record (its first 160 bytes) carries no health yet: the game works it
  // out on this client.
  std::memcpy(&report.damage, impact.contact_data.data() + 48, sizeof(report.damage));
  if (!(report.damage >= 0.0f) || !(report.damage <= kcd2s::integration::kMaxDamage)) {
    report.damage = 0;
  }
  held_contacts[report.request] = HeldContact{impact, GetTickCount64(), local_combat_life};
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!client->peer().send_player_hit(report)) {
      held_contacts.erase(report.request);
      return false;
    }
  }
  log("HIT %u from player %u held for the server's ruling (known health %.2f)", report.request,
      report.attacker, report.damage);
  return true;
}

// The held contacts the server ruled to take, with their verdicts; refused ones and ones left
// unruled too long are dropped.
std::vector<std::pair<kcd2s::integration::MeleeEvent, kcd2s::integration::HitVerdict>>
take_ruled_contacts(const std::vector<kcd2s::integration::HitVerdict>& verdicts) {
  std::vector<std::pair<kcd2s::integration::MeleeEvent, kcd2s::integration::HitVerdict>> ruled;
  const std::uint64_t now = GetTickCount64();
  for (const auto& verdict : verdicts) {
    auto found = held_contacts.find(verdict.request);
    if (found == held_contacts.end()) {
      continue;
    }
    if (now - found->second.held_at > kHitRulingMs) {
      log("HIT %u dropped: ruling expired before engine drain", verdict.request);
    } else if (found->second.life != local_combat_life) {
      log("HIT %u dropped: earlier local life", verdict.request);
    } else if (verdict.ruling == kcd2s::integration::HitRuling::Refused) {
      log("HIT %u refused by the server", verdict.request);
    } else {
      ruled.emplace_back(found->second.impact, verdict);
    }
    held_contacts.erase(found);
  }
  for (auto it = held_contacts.begin(); it != held_contacts.end();) {
    if (now - it->second.held_at > kHitRulingMs) {
      log("HIT %u dropped: no ruling", it->first);
      it = held_contacts.erase(it);
    } else {
      ++it;
    }
  }
  return ruled;
}

// The health a ruled blow takes, from what the game worked out.
float ruled_health(const kcd2s::integration::HitVerdict& verdict, float worked_out) {
  switch (verdict.ruling) {
    case kcd2s::integration::HitRuling::Set:
      return verdict.value;
    case kcd2s::integration::HitRuling::Scaled:
      return worked_out * verdict.value;
    default:
      return worked_out;
  }
}

std::optional<ActiveRuling> active_ruling;

QueueNativeHit original_queue_native_hit = nullptr;

// Arrows, bolts and balls from another player on the local player: the game takes
// their health and then queues their hit with that health (+ 48); while some resource handles
// playerHit the hit record waits for the server's ruling, by request. A ruling puts the health
// right (given back when refused, the difference when set) and the hit is queued again with the
// ruled health (its reaction, playerDamage); a refused one is not. One with no ruling after
// kHitRulingMs keeps what the game took. Engine thread.
struct HeldMissile {
  std::array<std::uint8_t, 160> hit{};
  void* rules = nullptr;
  std::uint64_t held_at = 0;
  std::uint64_t life = 0;
  std::uint32_t revision = 0;
  core::EntityId source = core::kInvalidEntity;
  core::EntityId target = core::kInvalidEntity;
  std::uint8_t source_epoch = 0;
  std::uint8_t target_epoch = 0;
  kcds::runtime::combat::DamageReports::Ticket damage_ticket = 0;
  std::uint64_t damage_receipt_sequence = 0;
  integration::PlayerDamage original_damage;
};
// The hit record's type (+ 24) of a projectile.
constexpr unsigned kProjectileHitType = 6;
std::unordered_map<std::uint32_t, HeldMissile> held_missiles;
// Whether blows wait for the server's ruling (the server's HIT_RULES), for the hit queue hook.
std::atomic<bool> hits_ruled{false};
bool requeuing_missile = false;

// Melee blows from other players on the local player while some resource handles playerHit
// the game works each out at once (the block, the reaction, the stamina it costs)
// and only its health waits for the server's ruling. The game's pricing step (see
// combat/priced_hits.hpp) is watched for a melee blow from a replica on the local player: its
// calculator, price input, stat query contexts and result are kept. When the game applies the
// blow's health effect (C_HealthValueEffect) it takes nothing yet, the blow's event is
// referenced and the blow goes to the server (PLAYER_HIT with the price). The ruled health is
// then applied by the same effect, the game's own way (its knock-out included); a Priced ruling
// first prices the kept calculator again with its modifiers. A blow with no ruling after
// kHitRulingMs takes what the game priced. Its playerDamage waits for the ruling. Engine thread.
PriceBlow original_price_blow = nullptr;
CapPrice cap_price = nullptr;
ApplyHealthEffect original_apply_health_effect = nullptr;
// the items the calculator names at + 32 and + 48 (by handle).
CalculatorItem calculator_item_32 = nullptr;
CalculatorItem calculator_item_48 = nullptr;
// A lethal blow's knock-out (the victim's combat soul, the blow's event, how long):
// it adds the unconsciousness (reason 1, C_AddBuffEffect) to the blow's event; the post-price step
// calls it for a lethal price, with the combat config's duration (+ 0x870).
KnockOut original_knock_out = nullptr;
CombatConfig combat_config = nullptr;
constexpr std::size_t kLethalKnockOutDuration = 0x870;
// Whether the pricing hooks are in place; without them blows wait before they are worked out.
bool priced_hits_ready = false;

struct PricedBlow {
  kcds::runtime::combat::Calculator calculator;
  kcds::runtime::combat::PriceInput input;
  kcds::runtime::combat::PriceResult result;
  // The attacker's and the victim's stat query contexts the price input pointed to.
  std::array<std::uint8_t, kcds::runtime::combat::kPriceContextBytes> attacker_context{};
  std::array<std::uint8_t, kcds::runtime::combat::kPriceContextBytes> victim_context{};
  bool contexts = false;
  float victim_stamina = 0;  // before the blow: the price reads it
  float priced = 0;          // the capped health of the game's price
  float stamina = 0;         // the capped stamina of the game's price
  void* event = nullptr;     // referenced while held
  void* health_effect = nullptr;
  std::int32_t subpart = -1;
  std::uint32_t attacker = 0;
  std::uint64_t priced_at = 0;
  std::uint64_t life = 0;
  std::uint32_t request = 0;  // once its health is held for a ruling
  std::uint64_t held_at = 0;
  float engine = 0;  // the health its effect was to take
  // The knock-out the game gave the blow while it priced it (a lethal price), held back with its
  // duration until the ruling.
  bool knockout = false;
  float knockout_duration = 0;
  bool damage_staged = false;
  kcds::runtime::combat::DamageReports::Ticket damage_ticket = 0;
  std::uint64_t damage_receipt_sequence = 0;
  integration::PlayerDamage damage;
};
std::vector<PricedBlow> priced_blows;

bool priced_request_used(std::uint32_t request) {
  for (const auto& blow : priced_blows) {
    if (blow.request == request) {
      return true;
    }
  }
  return false;
}

// The pending tables share one request namespace. Skip zero and live IDs on wrap.
std::uint32_t reserve_hit_request() {
  const std::size_t attempts =
      held_contacts.size() + held_missiles.size() + priced_blows.size() + 1;
  for (std::size_t index = 0; index < attempts; ++index) {
    const std::uint32_t request = next_hit_request++;
    if (!next_hit_request) {
      next_hit_request = 1;
    }
    if (request && !held_contacts.count(request) && !held_missiles.count(request) &&
        !priced_request_used(request)) {
      return request;
    }
  }
  return 0;
}

void finish_missile_damage(const HeldMissile& held, std::optional<float> amount) {
  std::lock_guard<std::mutex> lock(player_damage_mutex);
  if (amount && *amount > 0.0f && *amount <= integration::kMaxDamage) {
    auto damage = held.original_damage;
    damage.amount = *amount;
    if (player_damage.resolve(held.damage_ticket, damage)) {
      last_damage_ms = std::max<ULONGLONG>(last_damage_ms, held.held_at);
      last_damage_sequence = std::max(last_damage_sequence, held.damage_receipt_sequence);
    }
  } else {
    player_damage.resolve(held.damage_ticket, std::nullopt);
  }
}

// Engine-thread cancellation: historical contact data and GameRules pointers never survive a life
// or world replacement. An opponent-specific cancellation leaves unrelated pending contacts intact.
void discard_priced_blows(const char* reason, core::EntityId opponent);

void discard_held_hits(const char* reason, core::EntityId opponent) {
  // Without combat nothing was ever held.
  if (!combat_available.load()) {
    return;
  }
  discard_priced_blows(reason, opponent);
  const std::size_t before = held_contacts.size() + held_missiles.size();
  if (!opponent) {
    // Other harm held for a ruling goes with the life it belonged to.
    discard_held_harm(reason);
    held_contacts.clear();
    for (const auto& [request, missile] : held_missiles) {
      finish_missile_damage(missile, std::nullopt);
    }
    held_missiles.clear();
    active_ruling.reset();
    ++local_combat_life;
  } else {
    for (auto it = held_contacts.begin(); it != held_contacts.end();) {
      if (it->second.impact.source == opponent || it->second.impact.target == opponent) {
        it = held_contacts.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = held_missiles.begin(); it != held_missiles.end();) {
      if (it->second.source == opponent) {
        finish_missile_damage(it->second, std::nullopt);
        it = held_missiles.erase(it);
      } else {
        ++it;
      }
    }
  }
  const std::size_t removed = before - held_contacts.size() - held_missiles.size();
  if (removed) {
    log("HIT discarded pending=%zu reason=%s opponent=%u", removed, reason, opponent);
  }
}

// Holds a projectile's hit on the local player and asks the server; false when it is no other
// player's or too many wait.
bool hold_missile(void* rules, const std::uint8_t* hit, bool& damage_reported) {
  const std::uint32_t attacker = hit_attacker(hit);
  if (!attacker || held_missiles.size() >= kMaxHeldContacts) {
    return false;
  }
  kcd2s::integration::PlayerHitReport report;
  report.request = reserve_hit_request();
  if (!report.request) {
    return false;
  }
  report.attacker = attacker;
  report.missile = true;
  std::memcpy(&report.damage, hit + 48, sizeof(report.damage));
  if (!(report.damage >= 0.0f) || !(report.damage <= kcd2s::integration::kMaxDamage)) {
    report.damage = 0;
  }
  HeldMissile held;
  std::memcpy(held.hit.data(), hit, held.hit.size());
  held.rules = rules;
  held.held_at = GetTickCount64();
  held.life = local_combat_life;
  held.revision = requested_revision;
  held.source = attacker;
  held.target = local_network_id.load();
  const auto damage = hit_damage_report(hit);
  if (!damage) {
    return false;
  }
  held.original_damage = *damage;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    const auto source_epoch = client->peer().combat_epoch(held.source);
    const auto target_epoch = client->peer().combat_epoch(held.target);
    if (!source_epoch || !target_epoch) {
      return false;
    }
    held.source_epoch = *source_epoch;
    held.target_epoch = *target_epoch;
  }
  {
    std::lock_guard<std::mutex> lock(player_damage_mutex);
    const auto ticket = player_damage.stage_pending(*damage, held.held_at);
    if (!ticket) {
      log("DAMAGE report queue full; missile ruling not admitted");
      return false;
    }
    held.damage_ticket = *ticket;
    held.damage_receipt_sequence = ++damage_receipt_sequence;
  }
  held_missiles[report.request] = held;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!client->peer().send_player_hit(report)) {
      held_missiles.erase(report.request);
      finish_missile_damage(held, held.original_damage.amount);
      damage_reported = true;
      return false;
    }
  }
  log("HIT %u: a missile from player %u held for the server's ruling (health %.2f)", report.request,
      report.attacker, report.damage);
  return true;
}

bool current_held_missile(const HeldMissile& held) {
  if (held.life != local_combat_life || held.revision != requested_revision) {
    return false;
  }
  integration::MeleeEvent context;
  context.revision = held.revision;
  context.source = held.source;
  context.target = held.target;
  context.source_epoch = held.source_epoch;
  context.target_epoch = held.target_epoch;
  std::lock_guard<std::mutex> lock(network_mutex);
  return client->peer().current_combat_context(context);
}

void report_committed_missiles_before_knockout() {
  for (const auto& [request, held] : held_missiles) {
    if (current_held_missile(held)) {
      finish_missile_damage(held, held.original_damage.amount);
      log("HIT %u: committed missile damage resolved before native knockout", request);
    } else {
      finish_missile_damage(held, std::nullopt);
    }
  }
}

// Queues ruled missile feedback. Native HP is already committed by this legacy path; an expired
// ruling leaves that result and reports it once, only while its original life/context is current.
void rule_missiles(const std::vector<kcd2s::integration::HitVerdict>& verdicts) {
  for (const auto& verdict : verdicts) {
    auto found = held_missiles.find(verdict.request);
    if (found == held_missiles.end()) {
      continue;
    }
    HeldMissile held = found->second;
    held_missiles.erase(found);
    if (!current_held_missile(held)) {
      finish_missile_damage(held, std::nullopt);
      log("HIT %u: missile dropped after life/world/combat context changed", verdict.request);
      continue;
    }
    if (GetTickCount64() - held.held_at > kHitRulingMs) {
      finish_missile_damage(held, held.original_damage.amount);
      log("HIT %u: missile ruling expired; original native damage reported", verdict.request);
      continue;
    }
    float worked_out = 0;
    std::memcpy(&worked_out, held.hit.data() + 48, sizeof(worked_out));
    const bool refused = verdict.ruling == kcd2s::integration::HitRuling::Refused;
    const float ruled = refused ? 0.0f : ruled_health(verdict, worked_out);
    if (!(ruled >= 0.0f) || !(ruled <= integration::kMaxDamage)) {
      finish_missile_damage(held, held.original_damage.amount);
      log("HIT %u: invalid ruled amount; original native damage retained", verdict.request);
      continue;
    }
    // The game already took what it worked out: put the difference right.
    auto local = static_cast<std::uint8_t*>(local_combat_actor.load());
    auto soul = local ? *reinterpret_cast<std::uint8_t**>(local + 1640) : nullptr;
    if (soul && ruled != worked_out) {
      kcd2s::integration::StatRequest health;
      health.stat = 0;
      health.value = std::max(kcd2s::integration::kMinPositiveHealth * 2,
                              *reinterpret_cast<float*>(soul + 1800) + worked_out - ruled);
      kcds::runtime::stats::apply(soul, {health}, kcds::runtime::knockout::knocked_out());
    }
    if (refused) {
      finish_missile_damage(held, std::nullopt);
      log("HIT %u: missile refused by the server, %.2f health given back", verdict.request,
          worked_out);
      continue;
    }
    std::memcpy(held.hit.data() + 48, &ruled, sizeof(ruled));
    finish_missile_damage(held, ruled);
    requeuing_missile = true;
    original_queue_native_hit(held.rules, held.hit.data());
    requeuing_missile = false;
    log("HIT %u: missile taken as ruled: the game worked out %.2f health, ruled %.2f",
        verdict.request, worked_out, ruled);
  }
  const std::uint64_t now = GetTickCount64();
  for (auto it = held_missiles.begin(); it != held_missiles.end();) {
    if (now - it->second.held_at > kHitRulingMs) {
      if (current_held_missile(it->second)) {
        finish_missile_damage(it->second, it->second.original_damage.amount);
        log("HIT %u: missile without ruling; original native damage reported", it->first);
      } else {
        finish_missile_damage(it->second, std::nullopt);
        log("HIT %u: expired missile discarded after context changed", it->first);
      }
      it = held_missiles.erase(it);
    } else {
      ++it;
    }
  }
}

void* local_combat_soul() {
  auto local = static_cast<std::uint8_t*>(local_combat_actor.load());
  return local ? *reinterpret_cast<void**>(local + 1640) : nullptr;
}

template <class T>
T calculator_field(const kcds::runtime::combat::Calculator& calculator, std::size_t offset) {
  T value{};
  std::memcpy(&value, calculator.bytes.data() + offset, sizeof(value));
  return value;
}

// The C_Event's reference (its vtable + 24 and + 32).
void reference_event(void* event) {
  slot<void(__fastcall*)(void*)>(event, 3)(event);
}

void release_event(void* event) {
  slot<void(__fastcall*)(void*)>(event, 4)(event);
}

// The calculator's capped stamina and health for a result of its pricing step (on a
// copy).
std::pair<float, float> capped_price(const kcds::runtime::combat::Calculator& priced,
                                     const kcds::runtime::combat::PriceResult& result) {
  using namespace kcds::runtime::combat;
  Calculator calculator = priced;
  std::memcpy(calculator.bytes.data() + kCalculatorResult, result.bytes.data(),
              result.bytes.size());
  cap_price(calculator.bytes.data());
  return {calculator_field<float>(calculator, kCalculatorResult),
          calculator_field<float>(calculator, kCalculatorHealth)};
}

// the price of a blow. A melee blow from a replica on the local player is kept for
// the server's ruling while some resource handles playerHit.
void* __fastcall hooked_price_blow(void* calculator, void* result, void* input) {
  using namespace kcds::runtime::combat;
  auto* bytes = static_cast<std::uint8_t*>(calculator);
  void* victim = *reinterpret_cast<void**>(bytes + kCalculatorVictim);
  void* attacker_soul = *reinterpret_cast<void**>(bytes + kCalculatorAttacker);
  const auto kind = *reinterpret_cast<std::uint32_t*>(bytes + kCalculatorKind);
  // Where a melee blow on the local player landed, for its playerDamage.
  void* const health_effect = *reinterpret_cast<void**>(bytes + kCalculatorHealthEffect);
  if (kind == kMeleeCalculator && victim && victim == local_combat_soul() && health_effect) {
    std::memcpy(&priced_melee_subpart,
                static_cast<std::uint8_t*>(health_effect) + kHealthEffectSubpart,
                sizeof(priced_melee_subpart));
    priced_melee_subpart_ms = GetTickCount64();
  }
  // Where a blow on a server NPC landed, for its npcDamage.
  if (victim && health_effect) {
    if (const auto body = kcds::runtime::npcs::health::body_of_soul(victim)) {
      std::memcpy(&priced_npc_subpart,
                  static_cast<std::uint8_t*>(health_effect) + kHealthEffectSubpart,
                  sizeof(priced_npc_subpart));
      priced_npc = body->npc;
      priced_npc_ms = GetTickCount64();
    }
  }
  if (!priced_hits_ready || !hits_ruled.load() || kind != kMeleeCalculator || !victim ||
      victim != local_combat_soul() || priced_blows.size() >= kMaxHeldContacts) {
    return original_price_blow(calculator, result, input);
  }
  std::uint32_t attacker = 0;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    if (auto it = remote_combat_souls.find(attacker_soul); it != remote_combat_souls.end()) {
      attacker = std::uint32_t(it->second);
    }
  }
  if (!attacker) {
    return original_price_blow(calculator, result, input);
  }
  PricedBlow blow;
  std::memcpy(blow.calculator.bytes.data(), bytes, kCalculatorBytes);
  std::memcpy(blow.input.bytes.data(), input, kPriceInputBytes);
  auto* contexts = static_cast<std::uint8_t*>(input);
  auto* attacker_context = *reinterpret_cast<std::uint8_t**>(contexts + kPriceAttackerContext);
  auto* victim_context = *reinterpret_cast<std::uint8_t**>(contexts + kPriceVictimContext);
  if (attacker_context && victim_context) {
    std::memcpy(blow.attacker_context.data(), attacker_context, kPriceContextBytes);
    std::memcpy(blow.victim_context.data(), victim_context, kPriceContextBytes);
    blow.contexts = true;
  }
  blow.victim_stamina = *reinterpret_cast<float*>(static_cast<std::uint8_t*>(victim) + 1804);
  blow.event = *reinterpret_cast<void**>(bytes + kCalculatorEvent);
  blow.health_effect = *reinterpret_cast<void**>(bytes + kCalculatorHealthEffect);
  if (blow.health_effect) {
    std::memcpy(&blow.subpart,
                static_cast<std::uint8_t*>(blow.health_effect) + kHealthEffectSubpart,
                sizeof(blow.subpart));
  }
  blow.attacker = attacker;
  blow.priced_at = GetTickCount64();
  blow.life = local_combat_life;
  void* const returned = original_price_blow(calculator, result, input);
  if (!blow.event || !blow.health_effect) {
    return returned;
  }
  std::memcpy(blow.result.bytes.data(), result, kPriceResultBytes);
  const auto [stamina, health] = capped_price(blow.calculator, blow.result);
  blow.stamina = std::max(0.0f, stamina);
  blow.priced = std::max(0.0f, health);
  log("HIT priced: a blow from player %u priced at %.2f health, %.2f stamina", attacker,
      blow.priced, blow.stamina);
  priced_blows.push_back(blow);
  return returned;
}

// Sends a priced blow to the server; false when it cannot be held (it is then taken at once).
bool hold_priced_blow(PricedBlow& blow) {
  kcd2s::integration::PlayerHitReport report;
  report.request = reserve_hit_request();
  if (!report.request) {
    return false;
  }
  report.attacker = blow.attacker;
  report.damage = std::min(blow.engine, integration::kMaxDamage);
  report.stamina = std::min(blow.stamina, integration::kMaxDamage);
  report.priced = true;
  report.price =
      kcds::runtime::combat::hit_price_of(blow.calculator, blow.input, blow.result, blow.subpart);
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!client->peer().send_player_hit(report)) {
      return false;
    }
  }
  reference_event(blow.event);
  blow.request = report.request;
  blow.held_at = GetTickCount64();
  log("HIT %u: a blow from player %u held for the server's ruling (the game priced %.2f health, "
      "attack %.2f/%.2f/%.2f, armour %.2f/%.2f/%.2f, block %.2f, coefficient %.2f, subpart %d, "
      "blocked %u, perfect %u, broken %u)",
      report.request, report.attacker, report.damage, report.price.attack[0],
      report.price.attack[1], report.price.attack[2], report.price.armor[0], report.price.armor[1],
      report.price.armor[2], report.price.block_defense, report.price.body_coefficient,
      report.price.subpart, unsigned(report.price.blocked), unsigned(report.price.perfect_block),
      unsigned(report.price.block_broken));
  return true;
}

void finish_priced_damage(const PricedBlow& blow, float health) {
  if (!blow.damage_staged) {
    return;
  }
  std::lock_guard<std::mutex> lock(player_damage_mutex);
  if (health > 0.0f) {
    auto damage = blow.damage;
    damage.amount = health;
    if (player_damage.resolve(blow.damage_ticket, damage)) {
      last_damage_ms = std::max<ULONGLONG>(last_damage_ms, blow.priced_at);
      last_damage_sequence = std::max(last_damage_sequence, blow.damage_receipt_sequence);
    }
  } else {
    player_damage.resolve(blow.damage_ticket, std::nullopt);
  }
}

// a health effect applied to a soul. A priced blow's takes nothing until its ruling.
char __fastcall hooked_apply_health_effect(void* effect) {
  // A walking body bumping into the local player takes nothing from them (riding/trample.hpp).
  if (softened_bump_effect(static_cast<const std::uint8_t*>(effect))) {
    return 0;
  }
  auto found = std::find_if(priced_blows.begin(), priced_blows.end(), [&](const PricedBlow& blow) {
    return blow.health_effect == effect && !blow.request;
  });
  if (found == priced_blows.end()) {
    // A fall's health: the game's fall damage (C_FallDamageCause) is a
    // health effect with the reason fall (+ 68), applied here and never through the soul's damage
    // function, so it is reported from the health the local player lost.
    constexpr std::size_t kHealthEffectReason = 68;
    constexpr std::int32_t kFallReason = 7;
    std::int32_t reason = 0;
    std::memcpy(&reason, static_cast<std::uint8_t*>(effect) + kHealthEffectReason, sizeof(reason));
    // Harm a resource rules on (playerHarm, held_harm.cpp): what the effect takes off the local
    // player is kept back for the ruling, and reported once it is taken.
    float change = 0.0f;
    std::memcpy(&change,
                static_cast<std::uint8_t*>(effect) + kcds::runtime::combat::kHealthEffectAmount,
                sizeof(change));
    if (change < 0.0f && begin_harm_hold(std::uint32_t(reason), nullptr)) {
      const char applied = original_apply_health_effect(effect);
      std::int32_t subpart = -1;
      std::memcpy(&subpart,
                  static_cast<std::uint8_t*>(effect) + kcds::runtime::combat::kHealthEffectSubpart,
                  sizeof(subpart));
      end_harm_hold(std::uint32_t(reason), subpart, nullptr);
      return applied;
    }
    auto soul = static_cast<std::uint8_t*>(local_combat_soul());
    if (reason != kFallReason || !soul) {
      return original_apply_health_effect(effect);
    }
    const float before = *reinterpret_cast<float*>(soul + 1800);
    const char applied = original_apply_health_effect(effect);
    const float after = *reinterpret_cast<float*>(soul + 1800);
    if (before - after > 0.0f) {
      integration::PlayerDamage damage;
      damage.amount = before - after;
      damage.reason = std::uint8_t(kFallReason);
      std::lock_guard<std::mutex> lock(player_damage_mutex);
      last_damage_ms = GetTickCount64();
      last_damage_sequence = ++damage_receipt_sequence;
      if (!player_damage.push_ready(damage, last_damage_ms)) {
        log("DAMAGE report queue full; fall report not admitted");
      }
    }
    return applied;
  }
  auto amount = reinterpret_cast<float*>(static_cast<std::uint8_t*>(effect) +
                                         kcds::runtime::combat::kHealthEffectAmount);
  found->engine = std::max(0.0f, -*amount);
  if (!hold_priced_blow(*found)) {
    log("HIT a blow from player %u taken at once: ruling queue unavailable", found->attacker);
    finish_priced_damage(*found, found->engine);
    priced_blows.erase(found);
    return original_apply_health_effect(effect);
  }
  const float worked_out = *amount;
  *amount = 0.0f;
  const char applied = original_apply_health_effect(effect);
  *amount = worked_out;
  return applied;
}

// a lethal blow's knock-out. A priced blow's waits for its ruling (take_priced_blow).
void* __fastcall hooked_knock_out(void* combat_soul, void* event, float duration) {
  auto found = std::find_if(priced_blows.begin(), priced_blows.end(), [&](const PricedBlow& blow) {
    return blow.event == event && !blow.request;
  });
  if (found == priced_blows.end()) {
    return original_knock_out(combat_soul, event, duration);
  }
  found->knockout = true;
  found->knockout_duration = duration;
  log("HIT a lethal price from player %u: its knock-out (%.1f s) waits for the ruling",
      found->attacker, duration);
  return nullptr;
}

// A melee hit on the local player whose blow is priced and held: its playerDamage waits for the
// ruling. False when no such blow is waiting for it.
bool stage_priced_damage(const std::uint8_t* hit) {
  const std::uint32_t attacker = hit_attacker(hit);
  if (!attacker) {
    return false;
  }
  for (auto it = priced_blows.rbegin(); it != priced_blows.rend(); ++it) {
    if (it->attacker != attacker || it->damage_staged) {
      continue;
    }
    const auto damage = hit_damage_report(hit);
    if (!damage) {
      return false;
    }
    std::lock_guard<std::mutex> lock(player_damage_mutex);
    const auto ticket = player_damage.stage_pending(*damage, GetTickCount64());
    if (!ticket) {
      log("DAMAGE report queue full; priced blow report not admitted");
      return false;
    }
    it->damage = *damage;
    it->damage_ticket = *ticket;
    it->damage_receipt_sequence = ++damage_receipt_sequence;
    it->damage_staged = true;
    return true;
  }
  return false;
}

// The blow priced again by the game with a ruling's modifiers (its capped health), or nullopt
// when its players are no longer the ones it was priced for.
std::optional<float> reprice_blow(const PricedBlow& blow,
                                  const kcd2s::integration::HitModifiers& modifiers) {
  using namespace kcds::runtime::combat;
  auto victim = calculator_field<std::uint8_t*>(blow.calculator, kCalculatorVictim);
  auto attacker_soul = calculator_field<void*>(blow.calculator, kCalculatorAttacker);
  if (!victim || victim != local_combat_soul()) {
    return std::nullopt;
  }
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    auto it = remote_combat_souls.find(attacker_soul);
    if (it == remote_combat_souls.end() || std::uint32_t(it->second) != blow.attacker) {
      return std::nullopt;
    }
  }
  Calculator calculator = blow.calculator;
  PriceInput input = modified_input(blow.input, modifiers);
  // The stat query contexts as the game builds them: each player's soul and the
  // calculator's items, looked up again by handle.
  auto attacker_context = blow.attacker_context;
  auto victim_context = blow.victim_context;
  if (blow.contexts) {
    void* item_32 = calculator_item_32(calculator.bytes.data());
    void* item_48 = calculator_item_48(calculator.bytes.data());
    std::memcpy(attacker_context.data(), &attacker_soul, sizeof(void*));
    std::memcpy(attacker_context.data() + 8, &item_32, sizeof(void*));
    std::memcpy(attacker_context.data() + 16, &item_48, sizeof(void*));
    std::memcpy(victim_context.data(), &victim, sizeof(void*));
    std::memcpy(victim_context.data() + 8, &item_48, sizeof(void*));
    std::memcpy(victim_context.data() + 16, &item_32, sizeof(void*));
    void* attacker_pointer = attacker_context.data();
    void* victim_pointer = victim_context.data();
    std::memcpy(input.bytes.data() + kPriceAttackerContext, &attacker_pointer, sizeof(void*));
    std::memcpy(input.bytes.data() + kPriceVictimContext, &victim_pointer, sizeof(void*));
  }
  // The price reads the victim's stamina: the stamina before the blow, as the game priced it.
  auto stamina = reinterpret_cast<float*>(victim + 1804);
  const float stamina_now = *stamina;
  *stamina = blow.victim_stamina;
  PriceResult result;
  original_price_blow(calculator.bytes.data(), result.bytes.data(), input.bytes.data());
  *stamina = stamina_now;
  return capped_price(calculator, result).second;
}

// Applies a held blow's health as ruled (no ruling: as the game priced it) and lets its event go.
void take_priced_blow(const PricedBlow& blow, const kcd2s::integration::HitVerdict* verdict) {
  float health = blow.engine;
  float repriced = blow.priced;
  if (verdict && verdict->ruling == kcd2s::integration::HitRuling::Priced) {
    if (const auto price = reprice_blow(blow, verdict->modifiers)) {
      repriced = *price;
    } else {
      log("HIT %u: its players changed, priced again as the game priced it", blow.request);
    }
  }
  if (verdict) {
    health =
        kcds::runtime::combat::ruled_priced_health(*verdict, blow.engine, blow.priced, repriced);
  }
  auto soul = static_cast<std::uint8_t*>(local_combat_soul());
  const float before = soul ? *reinterpret_cast<float*>(soul + 1800) : 0.0f;
  if (health > 0.0f) {
    auto amount = reinterpret_cast<float*>(static_cast<std::uint8_t*>(blow.health_effect) +
                                           kcds::runtime::combat::kHealthEffectAmount);
    const float worked_out = *amount;
    *amount = -health;
    original_apply_health_effect(blow.health_effect);
    *amount = worked_out;
  }
  // The knock-out of a blow ruled lethal: the one the game held back while it priced the blow, when
  // the ruling keeps the blow as heavy as the game priced it (ruled_knockout), or a new one when
  // the ruling takes all the health left. The game's own knock-out adds the
  // unconsciousness to the blow's event, and its effect is applied as the game applies the event's
  // effects. No ruling in time: as the game priced it.
  kcd2s::integration::HitVerdict native;
  native.ruling = kcd2s::integration::HitRuling::Native;
  const bool lethal = kcds::runtime::combat::ruled_knockout(
      verdict ? *verdict : native, blow.knockout, blow.priced, repriced, health, before);
  if (lethal && soul) {
    float duration = blow.knockout_duration;
    if (!blow.knockout) {
      duration = *reinterpret_cast<float*>(combat_config() + kLethalKnockOutDuration);
    }
    void* effect = original_knock_out(soul + 240, blow.event, duration);
    if (effect) {
      slot<char(__fastcall*)(void*)>(effect, 3)(effect);
    }
    log("HIT %u: lethal as ruled (%.2f of %.2f health), knocked out for %.1f s (the game's %s)",
        blow.request, health, before, duration, blow.knockout ? "own" : "new");
  }
  release_event(blow.event);
  finish_priced_damage(blow, health);
  log("HIT %u taken %s: the game priced %.2f health, %.2f taken", blow.request,
      verdict ? "as ruled" : "without a ruling", blow.engine, health);
}

// Lets a priced blow go without taking anything.
void drop_priced_blow(const PricedBlow& blow, const char* reason) {
  if (blow.request) {
    release_event(blow.event);
  }
  finish_priced_damage(blow, 0.0f);
  log("HIT %u: a blow from player %u dropped: %s", blow.request, blow.attacker, reason);
}

void rule_priced_blows(const std::vector<kcd2s::integration::HitVerdict>& verdicts) {
  for (const auto& verdict : verdicts) {
    auto found =
        std::find_if(priced_blows.begin(), priced_blows.end(),
                     [&](const PricedBlow& blow) { return blow.request == verdict.request; });
    if (found == priced_blows.end()) {
      continue;
    }
    const PricedBlow blow = *found;
    priced_blows.erase(found);
    if (blow.life != local_combat_life) {
      drop_priced_blow(blow, "earlier local life");
    } else {
      take_priced_blow(blow, &verdict);
    }
  }
  // Held with no ruling in time: what the game priced. Priced but never applied: nothing.
  const std::uint64_t now = GetTickCount64();
  for (auto it = priced_blows.begin(); it != priced_blows.end();) {
    if (it->request && now - it->held_at > kHitRulingMs) {
      const PricedBlow blow = *it;
      it = priced_blows.erase(it);
      take_priced_blow(blow, nullptr);
    } else if (!it->request && now - it->priced_at > kHitRulingMs) {
      const PricedBlow blow = *it;
      it = priced_blows.erase(it);
      drop_priced_blow(blow, "its health was never applied");
    } else {
      ++it;
    }
  }
}

// Lets the priced blows go (from one opponent, or all of them) without taking anything.
void discard_priced_blows(const char* reason, core::EntityId opponent) {
  for (auto it = priced_blows.begin(); it != priced_blows.end();) {
    if (!opponent || it->attacker == opponent) {
      const PricedBlow blow = *it;
      it = priced_blows.erase(it);
      drop_priced_blow(blow, reason);
    } else {
      ++it;
    }
  }
}
// Player movement slot 200 gates the weapon contact callback
// For an unlocked attack at the player's soft target it reports a near miss
// ("combat:playerAttackNearMiss") and returns false, leaving the outcome to the target; live, the
// attacker's contacts on a replica stop there (allowed=0 without lock, 1 with lock). Contacts on
// remote players are detected by the attacker and relayed to the victim's owner, so
// the local attacker's contact on a replica proceeds to the owner contact relay. Every other call
// is unchanged.
PlayerContactAllowed original_player_contact_allowed = nullptr;
char __fastcall hooked_player_contact_allowed(void* movement, void* target) {
  // The near-miss branch also resets the attack power (core+480), so it must not run first.
  auto actor =
      movement ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(movement) + 504) : nullptr;
  const auto remote = target && actor && actor == local_combat_actor.load()
                          ? remote_combat_id(target)
                          : core::EntityId{};
  if (remote) {
    return 1;
  }
  return original_player_contact_allowed(movement, target);
}
std::intptr_t __fastcall hooked_queue_native_hit(void* rules, const void* info) {
  auto local = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto hit = static_cast<const std::uint8_t*>(info);
  // A trample is relayed to the trampled player's owner, and the rider's own copy still reaches
  // the replica through GameRules for its fall (the report of that reaction to the rider's horse is
  // skipped, see hooked_source_hit_report).
  if (!replaying_melee && hit && slow_ridden_horse_collision(hit)) {
    return 0;
  }
  // A walking body bumping into the local player, or the player into it: no blow.
  if (!replaying_melee && hit && softened_bump_hit(hit)) {
    return 0;
  }
  if (!replaying_melee && hit) {
    relay_trample(hit);
  }
  // A blow on the local player while they lie knocked out lands nowhere: on a KCDS server players
  // are only knocked out and the server decides when they get up (a bandit kept striking a
  // knocked-out player).
  if (hit && local &&
      *reinterpret_cast<const unsigned*>(hit + 40) ==
          *reinterpret_cast<const unsigned*>(local + 48)) {
    void* own_soul = *reinterpret_cast<void* const*>(local + 1640);
    if (kcds::runtime::knockout::knocked_out() ||
        (own_soul && kcds::runtime::knockout::creature_unconscious(own_soul))) {
      log("KNOCKOUT blow on the lying local player dropped source=%u",
          *reinterpret_cast<const unsigned*>(hit));
      return 0;
    }
  }
  // A server NPC's blow on the local player takes the share of its damage the server set.
  scale_npc_blow_on_local(local, const_cast<std::uint8_t*>(hit));
  // diagnostic: every HitInfo the game queues (a horse running into a character is
  // expected here), with its producer, until the trample path is known.
  if (hit) {
    char words[16 * 9 + 1] = {};
    for (int index = 0; index < 16; ++index) {
      std::snprintf(words + index * 9, 10, "%08x ",
                    *reinterpret_cast<const unsigned*>(hit + index * 4));
    }
    log("HIT queue source=%u target=%u flags=%x local=%u caller=%p words=%s",
        *reinterpret_cast<const unsigned*>(hit), *reinterpret_cast<const unsigned*>(hit + 40),
        *reinterpret_cast<const unsigned*>(hit + 120),
        local ? *reinterpret_cast<const unsigned*>(local + 48) : 0u, _ReturnAddress(), words);
  }
  // An NPC replica's own blow (an animal's replica runs its AI) lands nowhere: the NPC's blows
  // come from its simulator (NPC_BLOW).
  if (!replaying_melee && hit) {
    const auto striker =
        kcds::runtime::npcs::health::body_of_entity(*reinterpret_cast<const unsigned*>(hit));
    if (striker && !striker->simulated) {
      return 0;
    }
  }
  if (!replaying_melee && hit) {
    const bool on_npc_replica = take_npc_hit_health(local, const_cast<std::uint8_t*>(hit));
    relay_npc_blow(hit);
    if (on_npc_replica) {
      return 0;
    }
  }
  if (!replaying_melee && local && hit &&
      *reinterpret_cast<const unsigned*>(hit) == *reinterpret_cast<const unsigned*>(local + 48)) {
    const auto target = *reinterpret_cast<const unsigned*>(hit + 40);
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& entry : remote_combat_movements) {
      auto actor =
          *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(entry.second) + 504);
      if (actor && *reinterpret_cast<const unsigned*>(actor + 48) == target)
        return 0;
    }
  }
  // A replica's shot on another replica: only the game of the player it hits works
  // the hit out.
  if (!replaying_melee && hit) {
    const auto source = *reinterpret_cast<const unsigned*>(hit);
    const auto target = *reinterpret_cast<const unsigned*>(hit + 40);
    bool source_replica = false;
    bool target_replica = false;
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& entry : remote_combat_movements) {
      auto actor =
          *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(entry.second) + 504);
      const unsigned id = actor ? *reinterpret_cast<const unsigned*>(actor + 48) : 0;
      source_replica = source_replica || (id && id == source);
      target_replica = target_replica || (id && id == target);
    }
    if (source_replica && target_replica)
      return 0;
  }
  // Another player's arrow, bolt or ball on the local player waits for the server's ruling while
  // some resource handles playerHit.
  bool missile_damage_reported = false;
  if (!requeuing_missile && !replaying_melee && local && hit && hits_ruled.load() &&
      *reinterpret_cast<const unsigned*>(hit + 40) ==
          *reinterpret_cast<const unsigned*>(local + 48) &&
      *reinterpret_cast<const unsigned*>(hit + 24) == kProjectileHitType &&
      hold_missile(rules, hit, missile_damage_reported)) {
    return 0;
  }
  // A hit on the local player is health its own game takes: playerDamage. A ruled
  // blow's record carries the ruled health; a priced blow's waits for its ruling.
  if (local && hit &&
      *reinterpret_cast<const unsigned*>(hit + 40) ==
          *reinterpret_cast<const unsigned*>(local + 48)) {
    if (active_ruling && !active_ruling->queued) {
      auto health = reinterpret_cast<float*>(const_cast<std::uint8_t*>(hit) + 48);
      active_ruling->worked_out = std::max(0.0f, *health);
      active_ruling->ruled = ruled_health(active_ruling->verdict, active_ruling->worked_out);
      active_ruling->queued = true;
      *health = active_ruling->ruled;
    }
    // Not another player's hit, while harm is ruled: its health effect is held and reported then.
    const bool harm_ruled = !missile_damage_reported && hit_attacker(hit) == 0 &&
                            harm_hit_queued(*reinterpret_cast<const unsigned*>(hit));
    if (!missile_damage_reported && !harm_ruled && !stage_priced_damage(hit)) {
      queue_hit_damage(hit);
    }
  }
  return original_queue_native_hit(rules, info);
}

}  // namespace kcds::game_client
