// The kit's Esc in a game dialogue (KCDS_TEST_HOOKS only): the scripted input's step
// "dialog_cancel" leaves the dialogue the way the dialogue's cancel key does. The dialogue's screen
// registers its keys with the input manager when it opens (the action map "dialog"); its
// dialog_cancel handler posts the input 4 (cancel) to the screen's own queue at + 8. The
// registration is watched for the screen, and the step posts that input as the key's handler does.
#pragma once

namespace kcds::runtime::testing::dialog_cancel {

using Logger = void (*)(const char* format, ...);

// Watches the dialogue screen's key registration; false when its signatures are not found.
bool install(Logger logger);

// Leaves the open dialogue as its Esc does; false when no dialogue screen has registered its keys.
bool cancel();

}  // namespace kcds::runtime::testing::dialog_cancel
