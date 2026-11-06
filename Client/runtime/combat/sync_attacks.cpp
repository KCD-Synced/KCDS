// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "sync_attacks.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"
#include "attack_observers.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// MCP queues an original 80-byte actor action query.
// Observe scalar decision fields only; its +40 vector contains local objects.
QueueCombatQuery original_queue_combat_query;
std::intptr_t __fastcall hooked_queue_combat_query(void* self, std::uint8_t kind,
                                                   const void* query) {
  // dispatches Actor+408's v1, not the reaction component.
  // Never infer Movement from this arbitrary AI object's +8 identifier.
  auto movement = static_cast<std::uint8_t*>(observed_reaction_movement);
  auto actor = movement ? *reinterpret_cast<std::uint8_t**>(movement + 504) : nullptr;
  if (!actor || *reinterpret_cast<void**>(actor + 408) != self)
    return original_queue_combat_query(self, kind, query);
  bool local = actor == local_combat_actor.load();
  core::EntityId id = 0;
  if (!local) {
    auto soul = *reinterpret_cast<void**>(actor + 1640);
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    if (auto it = remote_combat_souls.find(soul); it != remote_combat_souls.end())
      id = it->second;
  }
  if ((local || id) && query && (kind == 3 || kind == 9 || kind == 11 || kind == 13)) {
    auto q = static_cast<const std::uint8_t*>(query);
    std::uint32_t strength = 0, mode = 0, material = 0;
    float distance = 0;
    std::memcpy(&strength, q, 4);
    std::memcpy(&mode, q + 4, 4);
    std::memcpy(&distance, q + 12, 4);
    std::memcpy(&material, q + 72, 4);
    log("COMBAT native reaction query local=%u replica=%u kind=%u action=%u strength=%u mode=%u "
        "can_block=%u other=%u flags=%u distance=%.5f material=%u",
        unsigned(local), id, unsigned(kind), native_action_type(movement), strength, mode,
        unsigned(q[8]), unsigned(q[9]), unsigned(q[10]), distance, material);
  }
  return original_queue_combat_query(self, kind, query);
}
std::uint8_t* native_sync_hit_database = nullptr;
std::uint8_t* native_sync_attack_database = nullptr;
std::unordered_map<core::EntityId, PendingSyncHit> pending_sync_hits;
thread_local void* replayed_combat_movement = nullptr;
void* find_native_sync_hit(const integration::NativeSyncHitData& data) {
  if (!native_sync_hit_database)
    return nullptr;
  auto begin = *reinterpret_cast<std::uint8_t**>(native_sync_hit_database + 48);
  auto end = *reinterpret_cast<std::uint8_t**>(native_sync_hit_database + 56);
  const auto size = reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(begin);
  if (!begin || size % 536 || size / 536 > 10000)
    return nullptr;
  void* found = nullptr;
  for (auto row = begin; row != end; row += 536)
    if (std::memcmp(row + 8, data.data(), 56) == 0) {
      if (found)
        return nullptr;
      found = row;
    }
  return found;
}
MakeSyncHit original_make_sync_hit;
void* __fastcall hooked_make_sync_hit(void* factory, void** out, void* row, bool flags,
                                      int context) {
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(factory) + 8);
  auto actor = movement ? *reinterpret_cast<void**>(movement + 504) : nullptr;
  bool local = actor && actor == local_combat_actor.load();
  core::EntityId id = 0;
  if (actor && !local) {
    auto soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 1640);
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    if (auto it = remote_combat_souls.find(soul); it != remote_combat_souls.end())
      id = it->second;
  }
  if (local) {
    auto source_move = static_cast<std::uint8_t*>(replayed_combat_movement);
    if (!source_move && movement) {
      auto core = *reinterpret_cast<std::uint8_t**>(movement + 528);
      source_move = core ? *reinterpret_cast<std::uint8_t**>(core + 3320) : nullptr;
    }
    auto source_actor =
        source_move ? *reinterpret_cast<std::uint8_t**>(source_move + 504) : nullptr;
    auto source_soul = source_actor ? *reinterpret_cast<void**>(source_actor + 1640) : nullptr;
    core::EntityId source_id = 0;
    {
      std::lock_guard<std::mutex> lock(combat_identity_mutex);
      if (auto it = remote_combat_souls.find(source_soul); it != remote_combat_souls.end())
        source_id = it->second;
    }
    if (auto it = pending_sync_hits.find(source_id); it != pending_sync_hits.end()) {
      if (GetTickCount64() - it->second.received <= 2000) {
        if (auto selected = find_native_sync_hit(it->second.data)) {
          row = selected;
          flags = it->second.data[56] != 0;
          std::memcpy(&context, it->second.data.data() + 57, 4);
          log("COMBAT SyncHit owner selection applied source=%u row=%p", source_id, row);
        } else
          log("COMBAT SyncHit owner selection rejected: missing/ambiguous native row source=%u",
              source_id);
      }
      pending_sync_hits.erase(it);
    }
  }
  auto result = original_make_sync_hit(factory, out, row, flags, context);
  if ((local || id) && row) {
    auto d = static_cast<const std::uint8_t*>(row);
    char prefix[65];
    for (unsigned i = 0; i < 32; ++i)
      sprintf_s(prefix + i * 2, sizeof(prefix) - i * 2, "%02x", unsigned(d[i]));
    log("COMBAT native SyncHit selected local=%u replica=%u row=%p prefix=%s flags=%u context=%d "
        "action=%p",
        unsigned(local), id, row, prefix, unsigned(flags), context, out ? *out : nullptr);
  }
  return result;
}
// MCP C61C28 chooses both records; C61F1C constructs/refcounts the
// real native SyncAttack. 939310 normally reruns selection on the replica,
// which can fall back to an ordinary Attack due to different pair geometry.
ChooseSyncAttack original_choose_sync_attack;
ConstructSyncAttack construct_sync_attack;
MakeAttack original_make_attack;
core::EntityId remote_combat_id(void* movement) {
  auto actor = movement
                   ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 504)
                   : nullptr;
  auto soul = actor ? *reinterpret_cast<void**>(actor + 1640) : nullptr;
  std::lock_guard<std::mutex> lock(combat_identity_mutex);
  auto it = remote_combat_souls.find(soul);
  return it == remote_combat_souls.end() ? 0 : it->second;
}
void* __fastcall hooked_choose_sync_attack(void* factory, void** out, const void* query) {
  auto move = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(factory) + 8);
  // Pair (sync) attacks are chosen by the attacker's owner and arrive as a SyncHit owner
  // selection, which hooked_make_attack applies without calling this. A replica reaching here has
  // no such selection; letting it choose natively made the defender's client start a pair the
  // attacker never started (replica action 22 after the defender was hit, while the
  // attacker kept a normal attack), so the replica missed the attacker's next attacks. Take the
  // native "no sync attack" result instead (LABEL_21:, which
  // only stores null).
  if (out && remote_combat_id(move)) {
    *out = nullptr;
    if (GetCurrentThreadId() == engine_update_thread.load()) {
      refused_pairs.pair_refused(reinterpret_cast<std::uintptr_t>(move));
    }
    static ULONGLONG next_log = 0;
    if (GetTickCount64() >= next_log) {
      next_log = GetTickCount64() + 500;
      log("COMBAT replica sync attack suppressed without owner selection source=%u",
          remote_combat_id(move));
    }
    return out;
  }
  auto result = original_choose_sync_attack(factory, out, query);
  auto actor = move ? *reinterpret_cast<void**>(move + 504) : nullptr;
  if (actor != local_combat_actor.load() || !out || !*out)
    return result;
  auto action = static_cast<std::uint8_t*>(*out);
  auto attack_row = *reinterpret_cast<std::uint8_t**>(action + 88);
  auto hit_row = *reinterpret_cast<std::uint8_t**>(action + 96);
  auto core = *reinterpret_cast<std::uint8_t**>(move + 528);
  auto target = core ? *reinterpret_cast<void**>(core + 3320) : nullptr;
  auto id = remote_combat_id(target);
  if (id && attack_row && hit_row) {
    integration::NativeSyncHitData data{};
    std::memcpy(data.data(), hit_row + 8, 56);
    std::memcpy(data.data() + 61, attack_row + 8, 56);
    std::memcpy(data.data() + 117, action + 124, 4);
    // The target's client and every watcher: a watcher's replica of the attacker plays the
    // same pair against its replica of the target instead of an ordinary attack.
    std::vector<core::EntityId> recipients{id};
    {
      std::lock_guard<std::mutex> lock(combat_identity_mutex);
      for (const auto& entry : remote_combat_souls) {
        if (entry.second != id) {
          recipients.push_back(entry.second);
        }
      }
    }
    for (const auto recipient : recipients) {
      bool sent;
      {
        std::lock_guard<std::mutex> lock(network_mutex);
        sent = client->peer().send_sync_hit(local_network_id.load(), recipient, data);
      }
      log("COMBAT native pair selection queue source=%u target=%u recipient=%u sent=%u attack=%p "
          "hit=%p",
          local_network_id.load(), id, recipient, unsigned(sent), attack_row, hit_row);
    }
  }
  return result;
}
void* find_native_sync_attack(const integration::NativeSyncHitData& data) {
  auto begin = *reinterpret_cast<std::uint8_t**>(native_sync_attack_database + 48);
  auto end = *reinterpret_cast<std::uint8_t**>(native_sync_attack_database + 56);
  auto size = reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(begin);
  if (!begin || size % 576 || size / 576 > 10000)
    return nullptr;
  void* found = nullptr;
  for (auto row = begin; row != end; row += 576)
    if (std::memcmp(row + 8, data.data() + 61, 56) == 0) {
      if (found)
        return nullptr;
      found = row;
    }
  return found;
}

}  // namespace kcds::game_client
