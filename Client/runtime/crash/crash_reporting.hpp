// Native crash reporting for the game-client DLL.
//
// The launcher hands its validated "sentry" deployment settings to the game
// through the inherited environment (see kcds/launcher/crash_handoff.hpp).
// start_crash_reporting() reads them and, only when a DSN was handed over,
// starts the same backend the launcher uses with:
//
//   - release kcds-game-client@<build version>;
//   - crashpad_handler.exe from the DLL's own directory;
//   - the per-user report database %LOCALAPPDATA%\KCDS\sentry\game-client;
//   - the tail (at most 512 KiB) of this process's client-<pid>.log, copied
//     at crash time and attached to the report (launcher/crash_logs.hpp).
//
// With KCDS_CRASH_TEST=client in the environment (verification only, see
// launcher/crash_test.hpp) a started backend is followed by a deliberate
// crash on a thread of its own after KCDS_CRASH_TEST_DELAY seconds.
//
// The game installs a top-level exception filter of its own while its system starts (WHGame
// 1.5.5: sets TopLevelExceptionFilter, which handles the crash itself and never calls
// the filter before it), so Crashpad's filter was replaced and no game crash was ever reported.
// Once the backend started, SetUnhandledExceptionFilter is hooked: a later caller's filter is
// noted but not installed, and Crashpad's stays the process's.
//
// Without a DSN nothing is initialised and nothing is sent. Call it only from
// the DLL's initialization thread, never from DllMain: starting the backend
// creates the handler process. The backend is never shut down, so no SDK code
// runs under loader lock at process exit.
#ifndef KCDS_RUNTIME_CRASH_REPORTING_HPP
#define KCDS_RUNTIME_CRASH_REPORTING_HPP

#include <string>

namespace kcds::runtime::crash {

// Starts crash reporting if the launcher handed over a DSN. module_directory
// is the DLL's directory (with or without a trailing separator). Returns one
// line describing the result for the client log; it never contains the DSN.
std::string start_crash_reporting(const std::wstring& module_directory);

}  // namespace kcds::runtime::crash

#endif  // KCDS_RUNTIME_CRASH_REPORTING_HPP
