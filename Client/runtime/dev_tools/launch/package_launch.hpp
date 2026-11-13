#pragma once
// Starting a Microsoft Store / Xbox app copy of the game with the KCDS client. Built only with
// the CMake option KCDS_MSSTORE_LAUNCH, which is off by default: whether releases start such
// copies is a separate decision.
//
// Windows starts a packaged game only as its package (by its application user model id), never
// as a plain process. To load the client before the game runs, this tool registers itself as the
// package's debugger for that one start (IPackageDebugSettings::EnableDebugging). Windows then
// creates the package's process suspended and runs
//   <this tool> --package-attach <dll> <result file> <owner pid> -p <pid> -tid <tid>
// which loads the client and resumes the thread. The registration is removed right after the
// start, and a later start that still finds it (the starting tool was killed) only resumes the
// game and removes it.
//
// When the package's entry is a launch helper rather than KingdomCome.exe, the helper is resumed
// and the client is loaded into the KingdomCome.exe the helper starts, as soon as it appears.
// That process is not suspended, so the client may arrive after the game loaded WHGame.dll.
#include <string>

#include "store_package.hpp"

namespace kcds::runtime::launch {

// Starts the package with arguments and the client dll; game is its KingdomCome.exe. When Windows
// refuses the start registration, the package starts as usual and the client is loaded into the
// game as soon as it appears. Returns the launch-client exit code: 0 started, 1 failed, 3 not
// startable (not registered for this user), 4 closed right away.
int launch_package(const StorePackage& package, const std::wstring& game, const std::wstring& dll,
                   const std::wstring& arguments);

// Whether this run is the debugger side of launch_package.
bool is_package_attach(int argc, wchar_t** argv);

// The debugger side: loads the client into the new process and resumes it. Returns 0 or 1.
int attach_package_process(int argc, wchar_t** argv);

}  // namespace kcds::runtime::launch
