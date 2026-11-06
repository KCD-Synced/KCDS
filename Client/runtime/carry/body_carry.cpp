#include "body_carry.hpp"

#include <windows.h>

#include <iterator>
#include <unordered_map>

namespace kcds::runtime::carry {
namespace {

using kcd2s::integration::BodyCarry;
using kcd2s::integration::BodyCarryReport;

using CorpseCarry = void*(__fastcall*)(void* actor);
using Grab = void(__fastcall*)(void* carry, std::uint32_t entity);
using Put = void(__fastcall*)(void* carry);

constexpr std::size_t kActorCorpseCarry = 392;
// C_ActorCarryCorpse: its state at + 104 (5 while it carries; IsCarryingCorpse).
constexpr std::size_t kCarryState = 104;
constexpr std::uint32_t kCarrying = 5;
// The carried body's entity id (the carried replica's entity id there while state 5).
constexpr std::size_t kCarriedEntity = 128;
constexpr std::size_t kGrabSlot = 56 / sizeof(void*);
constexpr std::size_t kPutSlot = 80 / sizeof(void*);
// A grab the game did not take (the body not down yet, the replica not there yet) is asked again
// this often.
constexpr ULONGLONG kRetryMs = 2000;

CorpseCarry corpse_carry = nullptr;
BodyLogger log_line = nullptr;

// Carries the server announced, carrier -> carried, and when each was last asked of the game.
struct Wanted {
  std::uint32_t carried = 0;
  ULONGLONG asked = 0;
};
std::unordered_map<std::uint32_t, Wanted> wanted;
// Carries that ended, carrier -> carried, while the carrier's replica still puts the body down:
// the body stays the game's to move until it lies (given back to its owner's pose at once,
// it dropped to the ground, then hung on the carrier in a T-pose for a second).
struct Dropping {
  std::uint32_t carried = 0;
  ULONGLONG since = 0;
};
std::unordered_map<std::uint32_t, Dropping> dropping;
// The longest a put-down is waited for, in ms (the game's takes about 1.7 s, live).
constexpr ULONGLONG kPutDownMs = 4000;
// C_ActorCarryCorpse state while it puts the body down (5 -> 7 -> 0, live).
constexpr std::uint32_t kPuttingDown = 7;
// The player whose body the local player's game carried last frame (0 for none).
std::uint32_t local_carried = 0;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

// Whether an actor's corpse carry is picking up or carrying this body (any state but 0): a pick-up
// takes longer than the retry, and a second grab during it started it over.
bool holding(void* actor, std::uint32_t entity) {
  auto* carry =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + kActorCorpseCarry);
  return carry && *reinterpret_cast<std::uint32_t*>(carry + kCarryState) != 0 &&
         *reinterpret_cast<std::uint32_t*>(carry + kCarriedEntity) == entity;
}

// The body an actor's corpse carry is picking up or carrying (any state but 0), 0 for none.
std::uint32_t held_entity(void* actor) {
  if (!actor) {
    return 0;
  }
  auto* carry =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + kActorCorpseCarry);
  if (!carry || *reinterpret_cast<std::uint32_t*>(carry + kCarryState) == 0) {
    return 0;
  }
  return *reinterpret_cast<std::uint32_t*>(carry + kCarriedEntity);
}

// The local corpse carry's state last frame, logged when it changes (the pick-up's timing).
std::uint32_t local_state = 0;

void grab(void* actor, std::uint32_t entity) {
  void* carry = corpse_carry(actor);
  if (carry) {
    slot<Grab>(carry, kGrabSlot)(carry, entity);
  }
}

void put_down(void* actor) {
  void* carry = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActorCorpseCarry);
  if (carry && carried_entity(actor) != 0) {
    slot<Put>(carry, kPutSlot)(carry);
  }
}

}  // namespace

bool install_body_carry(void* getter, BodyLogger logger) {
  corpse_carry = reinterpret_cast<CorpseCarry>(getter);
  log_line = logger;
  return corpse_carry != nullptr;
}

std::uint32_t carried_entity(void* actor) {
  if (!actor) {
    return 0;
  }
  auto* carry =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + kActorCorpseCarry);
  if (!carry || *reinterpret_cast<std::uint32_t*>(carry + kCarryState) != kCarrying) {
    return 0;
  }
  return *reinterpret_cast<std::uint32_t*>(carry + kCarriedEntity);
}

void pick_up(void* actor, std::uint32_t entity) {
  if (corpse_carry && actor && entity) {
    grab(actor, entity);
  }
}

bool body_held(std::uint32_t player, std::uint32_t entity, void* local_actor) {
  for (const auto& [carrier, want] : wanted) {
    if (want.carried == player) {
      return true;
    }
  }
  for (const auto& [carrier, drop] : dropping) {
    if (drop.carried == player) {
      return true;
    }
  }
  if (!local_actor || !entity) {
    return false;
  }
  auto* carry = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) +
                                                  kActorCorpseCarry);
  // Any state but 0 is a carry under way (a pick-up starts before the state reaches 5).
  return carry && *reinterpret_cast<std::uint32_t*>(carry + kCarryState) != 0 &&
         *reinterpret_cast<std::uint32_t*>(carry + kCarriedEntity) == entity;
}

std::vector<BodyCarryReport> tick(const std::vector<BodyCarry>& from_server,
                                  const Players& players) {
  std::vector<BodyCarryReport> reports;
  if (!corpse_carry) {
    return reports;
  }
  for (const BodyCarry& carry : from_server) {
    switch (carry.kind) {
      case BodyCarry::Kind::Carry:
        wanted[carry.carrier] = Wanted{carry.carried, 0};
        break;
      case BodyCarry::Kind::Drop: {
        wanted.erase(carry.carrier);
        const Body carrier = players.body_of(carry.carrier);
        if (carrier.actor && carry.carrier != players.local) {
          put_down(carrier.actor);
          dropping[carry.carrier] = Dropping{carry.carried, GetTickCount64()};
        }
        break;
      }
      case BodyCarry::Kind::PutDown:
      case BodyCarry::Kind::Refuse:
        if (carry.carrier == players.local && players.local_actor) {
          put_down(players.local_actor);
        }
        break;
    }
    if (log_line) {
      log_line("CARRY body %u: carrier=%u carried=%u", unsigned(carry.kind), carry.carrier,
               carry.carried);
    }
  }
  // Every other carrier's replica holds the body the server says it carries.
  const ULONGLONG now = GetTickCount64();
  for (auto it = dropping.begin(); it != dropping.end();) {
    const Body carrier = players.body_of(it->first);
    const bool done = !carrier.actor || held_entity(carrier.actor) == 0 ||
                      now - it->second.since >= kPutDownMs || wanted.count(it->first) != 0;
    it = done ? dropping.erase(it) : std::next(it);
  }
  for (auto& [carrier_id, want] : wanted) {
    if (carrier_id == players.local) {
      continue;
    }
    const Body carrier = players.body_of(carrier_id);
    const Body body = players.body_of(want.carried);
    if (!carrier.actor || !body.entity || holding(carrier.actor, body.entity) ||
        now - want.asked < kRetryMs) {
      continue;
    }
    want.asked = now;
    grab(carrier.actor, body.entity);
    if (log_line) {
      log_line("CARRY replica of player=%u picks up the body of player=%u", carrier_id,
               want.carried);
    }
  }
  if (players.local_actor && log_line) {
    auto* carry = *reinterpret_cast<std::uint8_t**>(
        static_cast<std::uint8_t*>(players.local_actor) + kActorCorpseCarry);
    const std::uint32_t state = carry ? *reinterpret_cast<std::uint32_t*>(carry + kCarryState) : 0;
    if (state != local_state) {
      log_line("CARRY local corpse carry state %u -> %u (entity %u)", local_state, state,
               held_entity(players.local_actor));
      local_state = state;
    }
  }
  // The local player's own carry, reported as its pick-up starts (every other game then picks
  // the body up with it, live: reported once it was on the shoulder, the others saw the body lie
  // through the whole pick-up and then jump onto the carrier) and as its put-down starts (the
  // others put the body down with it).
  std::uint32_t entity = held_entity(players.local_actor);
  if (entity && local_state == kPuttingDown) {
    entity = 0;
  }
  const std::uint32_t now_carried = entity ? players.player_of(entity) : 0;
  if (now_carried != local_carried) {
    if (local_carried) {
      reports.push_back(BodyCarryReport{BodyCarryReport::Kind::Ended, local_carried});
    }
    if (now_carried) {
      reports.push_back(BodyCarryReport{BodyCarryReport::Kind::Started, now_carried});
    }
    local_carried = now_carried;
  }
  return reports;
}

}  // namespace kcds::runtime::carry
