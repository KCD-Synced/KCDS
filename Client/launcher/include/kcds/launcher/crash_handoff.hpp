// Crash-reporting settings handed from the launcher to the game-client DLL.
//
// The launcher does not start the game directly: it runs
// kcd2s-launch-client.exe, which creates the game process. Both inherit the
// launcher's environment (the same mechanism that already gives the game its
// Steam app context), so the launcher exports the validated "sentry" settings
// of its deployment configuration as environment variables before starting
// the game, and the DLL reads them on its initialization thread:
//
//   KCDS_CRASH_DSN                  our DSN; unset means "no reporting"
//   KCDS_CRASH_ENVIRONMENT          the environment tag
//   KCDS_CRASH_ALLOW_HTTP_LOOPBACK  "1" when the configuration allows plain
//                                   HTTP to a loopback host (development)
//
// When reporting is disabled or the settings are invalid the launcher clears
// all three, so a stale value inherited from its own parent never reaches the
// game. The DLL validates the values again with crash::start(); the release
// tag is the DLL's own build version, never a handed-over value.
#ifndef KCDS_LAUNCHER_CRASH_HANDOFF_HPP
#define KCDS_LAUNCHER_CRASH_HANDOFF_HPP

#include <string>
#include <vector>

#include "kcds/launcher/crash.hpp"

namespace kcds::launcher::crash {

inline constexpr const char* kDsnVariable = "KCDS_CRASH_DSN";
inline constexpr const char* kEnvironmentVariable = "KCDS_CRASH_ENVIRONMENT";
inline constexpr const char* kAllowHttpLoopbackVariable = "KCDS_CRASH_ALLOW_HTTP_LOOPBACK";

// One variable to export. An empty value means "remove the variable".
struct HandoffVariable {
  std::string name;
  std::string value;
};

// The variables for settings: all three set when reporting is enabled with a
// valid DSN and environment, otherwise all three cleared.
std::vector<HandoffVariable> handoff_variables(const Settings& settings, bool allow_http_loopback);

// What the game-client DLL received.
struct Handoff {
  Settings settings;  // enabled only when a DSN was handed over
  bool allow_http_loopback = false;
};

// Rebuilds the settings from the raw variable values (empty when unset).
Handoff read_handoff(const std::string& dsn, const std::string& environment,
                     const std::string& allow_http_loopback);

#ifdef _WIN32
// Applies the variables to this process's environment, which every child
// process inherits. Returns false if any change failed.
bool export_handoff(const std::vector<HandoffVariable>& variables);

// Reads the variables from this process's environment.
Handoff read_handoff_from_environment();
#endif

}  // namespace kcds::launcher::crash

#endif  // KCDS_LAUNCHER_CRASH_HANDOFF_HPP
