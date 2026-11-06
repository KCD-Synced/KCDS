// Combat test hooks for the live kit (KCDS_TEST_HOOKS only): native attacks pressed in a loop, a
// native held block, and the master strike started without its skill roll.
#pragma once

#include <Windows.h>

namespace kcds::game_client {

extern HANDLE native_attack_test_event;
extern double native_attack_test_until;
extern double native_attack_test_next;
extern double native_attack_test_release;
void tick_native_attack_test(void* actor, double now);
void tick_native_block_test(void* actor, double now);
extern HANDLE master_strike_test_event;
void tick_master_strike_test(void* actor);

}  // namespace kcds::game_client
