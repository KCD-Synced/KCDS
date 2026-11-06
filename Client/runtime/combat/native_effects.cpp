// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "native_effects.hpp"

#include "../core/log.hpp"
#include "../appearance/outfit.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"

#include <cstring>
#include <vector>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// Native factories allocate/refcount/register effects with C_Event. Replay the
// original committed scalar results, never rerun C_CombatSoul's damage processor.
CreateNativeCause create_native_cause = nullptr;
CreateNativeEvent create_native_event = nullptr;
CreateValueEffect create_health_effect = nullptr, create_stamina_effect = nullptr;
CreateBuffEffect create_buff_effect = nullptr;
CreateUnconsciousParams create_unconscious_params = nullptr;
CreateInjuredParams create_injured_params = nullptr;
CreateMoraleParams create_morale_params = nullptr;
void *native_injured_params_type = nullptr, *native_morale_params_type = nullptr;
void *native_health_effect_type = nullptr, *native_stamina_effect_type = nullptr,
     *native_buff_effect_type = nullptr, *native_unconscious_params_type = nullptr;
// An item's wear (C_ItemHealthValueEffect: (unused, event, item WUID, change); the
// WUID at + 16, the change at + 24). The victim's game makes it for both fighters' pieces; the
// attacker's weapon's goes to the attacker's own game (owners' weapons never wore).
CreateValueEffect create_item_health_effect = nullptr;
void* native_item_health_effect_type = nullptr;
// A piece's break (C_ItemDestroyedEffect, factory: (unused, event, item WUID, holder
// soul id); the WUID at + 16, the holder at + 24). The victim's game rolls it on a blow for a piece
// of quality 1 or less; its Apply gives the holder the weapon's broken
// one. The attacker's goes to the attacker's game like its wear, so only the real weapon breaks
// and every screen sees its broken one through the outfit.
CreateItemDestroyedEffect create_item_destroyed_effect = nullptr;
void* native_item_destroyed_effect_type = nullptr;
constexpr std::size_t kItemWuid = 0x30;

// The WUID's piece among those the fighter of soul id `soul` wears or holds; null when none.
void* worn_item_by_wuid(std::uint64_t soul_id, std::uint64_t wuid) {
  void* actor = nullptr;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& [soul, movement] : remote_combat_movements) {
      if (*reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(soul) + 48) == soul_id) {
        actor = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(movement) + 504);
        break;
      }
    }
  }
  if (!actor) {
    return nullptr;
  }
  for (void* item : kcds::runtime::appearance::outfit::worn(actor)) {
    if (*reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(item) + kItemWuid) == wuid) {
      return item;
    }
  }
  return nullptr;
}
integration::NativeEffects capture_native_effects(void* event, std::uint64_t source,
                                                  std::uint64_t target) {
  integration::NativeEffects result;
  auto bytes = static_cast<std::uint8_t*>(event);
  auto begin = *reinterpret_cast<std::uint8_t***>(bytes + 16),
       end = *reinterpret_cast<std::uint8_t***>(bytes + 24);
  if (!begin || end < begin || end - begin > 128)
    return result;
  for (auto it = begin; it != end; ++it) {
    auto effect = *it;
    if (!effect)
      continue;
    auto type = *reinterpret_cast<void**>(effect);
    if (type &&
        (type == native_item_health_effect_type || type == native_item_destroyed_effect_type)) {
      // Item wear and breaks: the attacker's own pieces' were taken before the submit
      // (take_attacker_wear), the victim's (here, the local player's) apply here as in a game
      // alone.
      continue;
    }
    if (type != native_health_effect_type && type != native_stamina_effect_type &&
        type != native_buff_effect_type) {
      log("COMBAT native non-vital effect retained locally type=%p", type);
      continue;
    }
    const auto soul = *reinterpret_cast<std::uint64_t*>(effect + 16);
    integration::NativeEffectRecord record;
    record.soul_role = soul == source ? 1 : (soul == target ? 2 : 0);
    if (!record.soul_role)
      continue;
    if (type == native_buff_effect_type) {
      record.kind = 3;
      std::memcpy(record.value.data(), effect + 24, 32);
      auto parameters = *reinterpret_cast<std::uint8_t**>(effect + 56);
      if (parameters) {
        auto parameter_type = *reinterpret_cast<void**>(parameters);
        if (parameter_type == native_unconscious_params_type) {
          record.value[32] = 1;
          std::memcpy(record.value.data() + 36, parameters + 8, 8);
          record.value[44] = parameters[16];
        } else if (parameter_type == native_injured_params_type) {
          record.value[32] = 2;
          std::memcpy(record.value.data() + 36, parameters + 8, 8);
          record.value[44] = parameters[16];
          record.value[45] = parameters[17];
        } else if (parameter_type == native_morale_params_type) {
          record.value[32] = 3;
          std::memcpy(record.value.data() + 36, parameters + 8, 4);
          record.value[44] = parameters[12];
          record.value[45] = parameters[13];
        } else {
          log("COMBAT buff parameters unsupported type=%p", parameter_type);
          continue;
        }
      }
    } else {
      record.kind = type == native_health_effect_type ? 1 : 2;
      std::memcpy(record.value.data(), effect + 24, 8);
      record.value[8] = effect[40];
      if (record.kind == 1) {
        std::memcpy(record.value.data() + 12, effect + 48, 4);
        const auto attacker = *reinterpret_cast<std::uint64_t*>(effect + 56);
        record.value[16] = attacker == source ? 1 : (attacker == target ? 2 : 0);
      }
    }
    result.push_back(record);
  }
  return result;
}
// Before the victim's game submits a hit: the wear and the break of the attacker's own pieces
// (its weapon on the blow) are taken out of the event and go to the attacker's game, which wears
// or breaks its real weapon; the replica's piece follows its owner's (the outfit), so a weapon
// that breaks breaks on every screen at once (the replica's sword broke here while its
// owner's stayed).
integration::NativeEffects take_attacker_wear(void* event, std::uint64_t source) {
  integration::NativeEffects taken;
  auto bytes = static_cast<std::uint8_t*>(event);
  auto begin = *reinterpret_cast<std::uint8_t***>(bytes + 16),
       end = *reinterpret_cast<std::uint8_t***>(bytes + 24);
  if (!native_item_health_effect_type || !begin || end < begin || end - begin > 128) {
    return taken;
  }
  for (auto it = begin; it != end; ++it) {
    auto effect = *it;
    if (!effect) {
      continue;
    }
    const auto type = *reinterpret_cast<void**>(effect);
    const bool wear = type == native_item_health_effect_type;
    const bool broken =
        native_item_destroyed_effect_type && type == native_item_destroyed_effect_type;
    if (!wear && !broken) {
      continue;
    }
    void* item = worn_item_by_wuid(source, *reinterpret_cast<std::uint64_t*>(effect + 16));
    auto klass =
        item ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(item) + 72) : nullptr;
    if (!klass) {
      continue;
    }
    integration::NativeEffectRecord record;
    record.kind = wear ? 4 : 5;
    record.soul_role = 1;
    std::memcpy(record.value.data(), klass + 8, 16);
    if (wear) {
      std::memcpy(record.value.data() + 16, effect + 24, 4);
      *reinterpret_cast<float*>(effect + 24) = 0.0f;
    } else {
      // No soul has id 0: the Apply finds no holder and leaves the replica's piece as it is.
      *reinterpret_cast<std::uint64_t*>(effect + 24) = 0;
      log("COMBAT attacker weapon break taken to its owner source=%llu",
          static_cast<unsigned long long>(source));
    }
    taken.push_back(record);
  }
  return taken;
}

bool populate_native_effects(void* event, const integration::NativeEffects& effects,
                             std::uint64_t source, std::uint64_t target, std::uint64_t local_soul) {
  for (const auto& record : effects) {
    auto soul = record.soul_role == 1 ? source : target;
    // Replicas follow owner state. A late health delta or unconsciousness/bleeding buff can
    // otherwise damage or knock out a replica after its owner already healed or revived.
    // Every effect on this client's own fighter still uses the original native factories.
    if (soul != local_soul) {
      continue;
    }
    std::uint8_t* effect = nullptr;
    if (record.kind == 4 || record.kind == 5) {
      // The local fighter's own piece of that class wears or breaks through the game's own effect.
      if (!create_item_health_effect || (record.kind == 5 && !create_item_destroyed_effect)) {
        continue;
      }
      void* local_actor = local_combat_actor.load();
      void* piece = nullptr;
      for (void* item : kcds::runtime::appearance::outfit::worn(local_actor)) {
        auto klass = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(item) + 72);
        if (klass && std::memcmp(klass + 8, record.value.data(), 16) == 0) {
          piece = item;
          break;
        }
      }
      if (!piece) {
        continue;
      }
      const auto wuid =
          *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(piece) + kItemWuid);
      if (record.kind == 5) {
        log("COMBAT own weapon break from the victim's game");
        if (!create_item_destroyed_effect(nullptr, event, wuid, local_soul))
          return false;
        continue;
      }
      float change;
      std::memcpy(&change, record.value.data() + 16, 4);
      if (!create_item_health_effect(nullptr, event, wuid, change))
        return false;
      continue;
    }
    if (record.kind < 3) {
      float value;
      std::memcpy(&value, record.value.data() + 4, 4);
      auto factory = record.kind == 1 ? create_health_effect : create_stamina_effect;
      effect = static_cast<std::uint8_t*>(factory(nullptr, event, soul, value));
      if (!effect)
        return false;
      effect[40] = record.value[8];
      if (record.kind == 1) {
        std::memcpy(effect + 48, record.value.data() + 12, 4);
        if (record.value[16])
          *reinterpret_cast<std::uint64_t*>(effect + 56) = record.value[16] == 1 ? source : target;
      }
    } else {
      alignas(16) std::array<std::uint8_t, 32> values{};
      std::memcpy(values.data(), record.value.data(), 32);
      effect = static_cast<std::uint8_t*>(create_buff_effect(nullptr, event, soul, values.data()));
      if (!effect)
        return false;
      std::memcpy(effect + 40, values.data() + 16, 16);
      if (record.value[32]) {
        float duration;
        unsigned mode;
        std::memcpy(&duration, record.value.data() + 36, 4);
        std::memcpy(&mode, record.value.data() + 40, 4);
        void* parameters = nullptr;
        const char flag = char(record.value[44]), extra = char(record.value[45]);
        if (record.value[32] == 1) {
          create_unconscious_params(&parameters, &duration, &mode);
          if (parameters)
            static_cast<std::uint8_t*>(parameters)[16] = record.value[44];
        } else if (record.value[32] == 2)
          create_injured_params(&parameters, &duration, &mode, &flag, &extra);
        else {
          unsigned context;
          std::memcpy(&context, record.value.data() + 36, 4);
          create_morale_params(&parameters, &context, &flag, &extra);
        }
        if (!parameters)
          return false;
        *reinterpret_cast<void**>(effect + 56) =
            parameters;  // native AddBuffEffect owns and destroys these params
      }
    }
  }
  return true;
}

}  // namespace kcds::game_client
