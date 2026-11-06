// The game's own RPG effects of a hit (health, stamina, buffs, item wear and breaks): captured from
// the event the victim's game made, the attacker's pieces' wear and breaks taken to its owner, and
// the same effects made again from their committed values.
#pragma once

#include <cstdint>

#include "kcd2s/integration/combat.hpp"

namespace kcds::game_client {

// The native factories and the effect and parameter types they make (resolved at startup).
using CreateNativeCause = void*(__fastcall*)(void**, const void*);
using CreateNativeEvent = void*(__fastcall*)(void*, void**, void**);
using CreateValueEffect = void*(__fastcall*)(void*, void*, std::uint64_t, float);
using CreateBuffEffect = void*(__fastcall*)(void*, void*, std::uint64_t, const void*);
using CreateUnconsciousParams = void*(__fastcall*)(void**, const float*, const unsigned*);
using CreateInjuredParams = void*(__fastcall*)(void**, const float*, const unsigned*, const char*,
                                               const char*);
using CreateMoraleParams = void*(__fastcall*)(void**, const unsigned*, const char*, const char*);
using CreateItemDestroyedEffect = void*(__fastcall*)(void*, void*, std::uint64_t, std::uint64_t);
extern CreateNativeCause create_native_cause;
extern CreateNativeEvent create_native_event;
extern CreateValueEffect create_health_effect;
extern CreateValueEffect create_stamina_effect;
extern CreateBuffEffect create_buff_effect;
extern CreateUnconsciousParams create_unconscious_params;
extern CreateInjuredParams create_injured_params;
extern CreateMoraleParams create_morale_params;
extern void* native_injured_params_type;
extern void* native_morale_params_type;
extern void* native_health_effect_type;
extern void* native_stamina_effect_type;
extern void* native_buff_effect_type;
extern void* native_unconscious_params_type;
extern CreateValueEffect create_item_health_effect;
extern void* native_item_health_effect_type;
extern CreateItemDestroyedEffect create_item_destroyed_effect;
extern void* native_item_destroyed_effect_type;

void* worn_item_by_wuid(std::uint64_t soul_id, std::uint64_t wuid);
kcd2s::integration::NativeEffects capture_native_effects(void* event, std::uint64_t source,
                                                         std::uint64_t target);
kcd2s::integration::NativeEffects take_attacker_wear(void* event, std::uint64_t source);
bool populate_native_effects(void* event, const kcd2s::integration::NativeEffects& effects,
                             std::uint64_t source, std::uint64_t target, std::uint64_t local_soul);

}  // namespace kcds::game_client
