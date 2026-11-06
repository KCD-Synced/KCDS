// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "cookpot_tick.hpp"

#include <cstdio>
#include <map>
#include <vector>

#include "../core/log.hpp"
#include "../level/level_edits.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/level_edits.hpp"
#include "kettle_state.hpp"
#include "pot_looks.hpp"

namespace kcds::game_client {
namespace {

namespace messages = kcd2s::integration;

bool shown_online = false;

// The fires the server holds (CookPot.setFire): the fireplace's level key -> lit.
// The fireplace's own behaviour still lights or puts out its fire (villagers stoking it, its burn
// out), so a held fire is told again every kHeldFireRetellMs.
std::map<std::uint64_t, bool> held_fires;
ULONGLONG fires_told_at = 0;
constexpr ULONGLONG kHeldFireRetellMs = 2000;

void tell_held_fires() {
  std::vector<std::uint64_t> keys;
  for (const auto& [key, lit] : held_fires) {
    keys.push_back(key);
  }
  const auto fireplaces = kcds::runtime::level::entities_by_guid(keys);
  for (const auto& [key, lit] : held_fires) {
    const auto fireplace = fireplaces.find(key);
    if (fireplace != fireplaces.end()) {
      kcds::runtime::cookpot::kettle::tell_fire(fireplace->second, lit);
    }
  }
  fires_told_at = GetTickCount64();
}

// A level EntityGuid (16 hex digits as the level files write it) as the entity system keys it.
std::uint64_t entity_key(std::uint64_t guid) {
  char digits[17] = {};
  std::snprintf(digits, sizeof(digits), "%016llx", static_cast<unsigned long long>(guid));
  std::uint64_t key = 0;
  messages::parse_level_edit_guid(digits, key);
  return key;
}

// The looks' cauldrons this level has, in their phases, and their fireplaces' kettles told the
// same fill (the game's meal check reads the kettle, kettle_state.hpp).
void show_looks(const messages::PotLooks& looks) {
  std::vector<std::uint64_t> keys;
  std::vector<std::uint64_t> fireplace_keys;
  keys.reserve(looks.looks.size());
  fireplace_keys.reserve(looks.looks.size());
  for (const messages::PotLook& look : looks.looks) {
    keys.push_back(entity_key(look.cauldron));
    if (look.fireplace != 0) {
      fireplace_keys.push_back(entity_key(look.fireplace));
    }
  }
  const auto found = kcds::runtime::level::entities_by_guid(keys);
  const auto fireplaces = kcds::runtime::level::entities_by_guid(fireplace_keys);
  unsigned shown = 0;
  unsigned told = 0;
  for (std::size_t i = 0; i < looks.looks.size(); ++i) {
    const messages::PotLook& look = looks.looks[i];
    const auto entity = found.find(keys[i]);
    if (entity == found.end()) {
      continue;
    }
    const std::uint32_t id = kcds::runtime::props::entity::id_of(entity->second);
    if (kcds::runtime::cookpot::show(id, look.camp, look.phase)) {
      ++shown;
    }
    if (look.fireplace == 0) {
      continue;
    }
    const std::uint64_t fireplace_key = entity_key(look.fireplace);
    const auto fireplace = fireplaces.find(fireplace_key);
    if (fireplace != fireplaces.end() &&
        kcds::runtime::cookpot::kettle::tell(fireplace->second, look.phase)) {
      ++told;
    }
    // The server's fire, held; the game's own again once the server hands it back.
    if (look.fire == messages::kPotFireOwn) {
      held_fires.erase(fireplace_key);
    } else {
      const bool lit = look.fire == messages::kPotFireLit;
      held_fires[fireplace_key] = lit;
      if (fireplace != fireplaces.end()) {
        kcds::runtime::cookpot::kettle::tell_fire(fireplace->second, lit);
      }
      log("POTS fireplace %016llx fire held %s", static_cast<unsigned long long>(look.fireplace),
          lit ? "lit" : "out");
    }
  }
  log("POTS %u of %zu looks shown here (%zu cauldrons found), %u kettles told", shown,
      looks.looks.size(), found.size(), told);
}

}  // namespace

// The server's looks of the pots in the local player's world go to the game's cauldrons; out of a
// server's world the game's own looks hold again.
void tick_cookpot_looks() {
  std::vector<messages::PotLooks> looks;
  bool online = false;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    online = client->peer().world_ready();
    looks = client->peer().take_pot_looks();
  }
  if (shown_online && !online) {
    kcds::runtime::cookpot::release_all();
    kcds::runtime::cookpot::kettle::forget_all();
    held_fires.clear();
  }
  shown_online = online;
  for (const messages::PotLooks& batch : looks) {
    show_looks(batch);
  }
  if (online && !held_fires.empty() && GetTickCount64() - fires_told_at >= kHeldFireRetellMs) {
    tell_held_fires();
  }
}

}  // namespace kcds::game_client
