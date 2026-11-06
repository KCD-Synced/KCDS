// Dodges on every screen: the owner's dodge relayed with its direction, and the same native
// dodge made on its replica by the replica's own dodge request.
#pragma once

#include <cstdint>

#include "kcd2s/core/ids.hpp"

namespace kcds::game_client {

// The dodge's creation (hooked: the original) and the controller's dodge request.
using CreateDodge = void**(__fastcall*)(void* movement, void** action, const void* row,
                                        void* filter);
extern CreateDodge original_create_dodge;
using RequestDodge = void(__fastcall*)(void* controller);
extern RequestDodge request_dodge;
void** __fastcall hooked_create_dodge(void* movement, void** action, const void* row, void* filter);
void start_replica_dodge(kcd2s::core::EntityId player, float degrees);
// When the local fighter's game last made a dodge in its slot (a dodge window open; GetTickCount64
// ms, 0 for never), and the dodge windows open on the local fighter now (dodges.hpp
// answer_contact).
std::uint64_t local_slot_dodge_at();
int local_dodge_windows(void* local_actor);

}  // namespace kcds::game_client
