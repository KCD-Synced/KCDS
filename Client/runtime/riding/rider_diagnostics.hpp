// diagnostics: body positions set and the local rider's stand-up, logged with their
// native callers while the local player rides.
#pragma once

#include <atomic>
#include <cstdint>

namespace kcds::game_client {

using BodyPositionSet = std::int64_t(__fastcall*)(void* positions, const int* position);
using BodyPositionStand = void(__fastcall*)(void* action, int result);
extern BodyPositionSet original_body_position_set;
extern BodyPositionStand original_body_position_stand;
extern std::atomic<bool> local_rider_mounted;
// Logs the game's own callers of the current call (frames inside WHGame.dll, as addresses).
void log_native_callers(const char* tag);
std::int64_t __fastcall hooked_body_position_set(void* positions, const int* position);
void __fastcall hooked_body_position_stand(void* action, int result);

}  // namespace kcds::game_client
