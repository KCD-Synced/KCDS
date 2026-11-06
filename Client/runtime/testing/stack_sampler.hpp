// Test hook (KCDS_TEST_HOOKS only; Local\KCD2S.StackSampler.<pid>): samples the engine thread's
// call stack, to name the functions a frame's time goes to. Each signal samples for
// the seconds in %TEMP%\kcds-stacks-<pid>.seconds (10 without it), writing one stack a line,
// innermost first, as module+offset to %TEMP%\kcds-stacks-<pid>.txt.
#pragma once

namespace kcds::game_client {

// Creates the hook's event; called by install_test_hooks with KCDS_TEST_HOOKS.
void install_stack_sampler();

// Notes the engine thread and starts a sampling a signal asked for. Engine thread, every frame.
void tick_stack_sampler();

}  // namespace kcds::game_client
