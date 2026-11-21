// A replica's native lifecycle as a remote player: its C_Player made with the remote role's vtable
// (C_Actor's lifecycle entries, no singleton UI or input registration), and its AIObject's
// transform subscriptions and AI grid entry removed before the object goes.
// remote_player_table is declared in core/client_internal.hpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::game_client {

// C_AIObject's constructor and destructor (hooked), the transform manager's cancel and the AI
// module's grid removal.
using AIObjectCtor = void*(__fastcall*)(void*, const void*, void*);
using AIObjectDtor = std::int64_t(__fastcall*)(void*);
using CancelTransform = std::int64_t(__fastcall*)(void*, void*, int, const void*);
extern AIObjectCtor original_ai_object_ctor;
extern AIObjectDtor original_ai_object_dtor;
extern CancelTransform cancel_transform;
extern void** transform_manager_slot;
using AIGridRemove = void(__fastcall*)(void*, void*);
extern std::uint8_t** ai_module_slot;
extern AIGridRemove ai_grid_remove;
void* __fastcall hooked_ai_object_ctor(void* self, const void* id, void* owner);
// Cancels every transform subscription whose owner lies in the `size` bytes at `object` (the
// object and the interfaces inside it), before the object is freed: the manager's update, on a
// worker thread, asks each owner for its position. Returns how many were cancelled.
std::size_t cancel_transform_subscriptions(const void* object, std::size_t size);
// The bytes of a C_Player (its actor creator's allocation).
constexpr std::size_t kPlayerBytes = 3584;
std::int64_t __fastcall hooked_ai_object_dtor(void* self);

// C_Player's constructor (hooked) and the C_Actor entries the remote role takes.
using PlayerCtor = void*(__fastcall*)(void*);
extern PlayerCtor original_player_ctor;
extern void* actor_hit;
extern void* actor_deactivate;
extern void* actor_update;
extern void* actor_tick;
extern void* actor_reset;
extern void* actor_init;
extern void* actor_post_init;
extern void* actor_release;
extern void* actor_event;
extern void* actor_activate;
void* __fastcall hooked_player_ctor(void* self);

}  // namespace kcds::game_client
