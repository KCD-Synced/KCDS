// Lets several game processes started by our launcher run side by side (two clients on one PC
// for development and tests), without changing KingdomCome.exe on disk.
//
// KingdomCome.exe creates a named mutex at startup and quits when GetLastError() reports
// ERROR_ALREADY_EXISTS (0xB7). The DLL turns that conditional jump into an unconditional one in
// the process's own memory, while the main thread is still suspended. A game started from Steam
// keeps the original single-instance check.
#ifndef KCDS_RUNTIME_MULTIPLE_INSTANCES_HPP
#define KCDS_RUNTIME_MULTIPLE_INSTANCES_HPP

namespace kcds::runtime::instances {

using Logger = void (*)(const char* format, ...);

// Call from DllMain before the game's main thread runs. Returns false, and logs why, when the
// check is not found exactly once in KingdomCome.exe or its code cannot be made writable; the game
// then keeps its single-instance check.
bool allow_multiple_instances(Logger logger);

}  // namespace kcds::runtime::instances

#endif  // KCDS_RUNTIME_MULTIPLE_INSTANCES_HPP
