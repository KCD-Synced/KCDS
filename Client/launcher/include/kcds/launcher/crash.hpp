// Crash-reporting configuration and the native crash-capture interface.
//
// The launcher and the mod report crashes with the Sentry native SDK to our own
// ingestion (a crash bridge at /api/{projectId}/envelope|minidump/, which
// forwards to the configured Sentry project). This module provides:
//
//   - the deployment settings ("sentry": enabled, dsn, environment) and a
//     strict DSN parser, so a malformed or plain-HTTP DSN is refused instead of
//     silently reporting somewhere unexpected;
//   - the Backend interface a native crash handler implements (the pinned SDK
//     with its Crashpad backend is crash::SentryBackend in sentry_backend.hpp,
//     built when KCDS_ENABLE_SENTRY is on);
//   - start(), which never initialises a backend unless reporting is
//     explicitly enabled with a valid DSN.
//
// There is no built-in or default DSN: without one nothing is initialised,
// captured or sent. A build without the SDK passes no backend, and an enabled
// configuration then yields BackendUnavailable. Gameplay tracking, session
// replay and performance tracing are out of scope; automatic attachments are
// limited to what the backend documents (never tokens, resource source or full
// process memory; logs only as the bounded crash-time tail of crash_logs.hpp).
#ifndef KCDS_LAUNCHER_CRASH_HPP
#define KCDS_LAUNCHER_CRASH_HPP

#include <string>

namespace kcds::launcher::crash {

// The "sentry" section of the launcher deployment configuration.
struct Settings {
  bool enabled = false;  // reporting is off unless explicitly enabled
  std::string dsn;       // our ingestion DSN; empty means none configured
  std::string environment = "production";
};

// A parsed DSN: {scheme}://{public_key}@{host[:port]}/{project_id}.
struct Dsn {
  std::string origin;      // normalised scheme://host[:port]
  std::string public_key;  // 16..128 ASCII letters/digits, as the crash bridge requires
  std::string project_id;  // decimal digits
};

// Parses and validates a DSN. The origin follows the update-feed rule (HTTPS;
// plain HTTP only to a loopback host when allow_http_loopback is set for
// development). A secret key, a path prefix, a query or a fragment is refused.
bool parse_dsn(const std::string& dsn, bool allow_http_loopback, Dsn& out);

// True for 1..64 characters of [a-z0-9_.-], the release/environment tag form.
bool is_tag(const std::string& value);

// Identifies reports from this process.
struct RuntimeInfo {
  std::string release;       // e.g. "kcds-launcher@0.1.1"; tags every report
  std::string database_dir;  // local directory for the backend's pending reports
};

// A native crash handler (for example the Sentry native SDK with a crash
// backend). Implementations must install their handler outside DLL loader
// lock and must not attach anything beyond the documented minimal report.
class Backend {
 public:
  virtual ~Backend() = default;

  // Installs the handler. Returns false if it could not be installed.
  virtual bool start(const Dsn& dsn, const Settings& settings, const RuntimeInfo& info) = 0;

  // Flushes and uninstalls the handler.
  virtual void stop() = 0;
};

enum class State {
  Disabled,            // not enabled in configuration: no backend was touched
  InvalidSettings,     // enabled, but the DSN or environment is not valid
  BackendUnavailable,  // enabled and valid, but this build has no crash backend
  Started,             // the backend installed its handler
  StartFailed,         // the backend refused to start
};

// Starts crash reporting if, and only if, settings enable it with a valid DSN.
// backend may be null (no SDK in this build). The backend is never called when
// the result is Disabled or InvalidSettings.
State start(const Settings& settings, bool allow_http_loopback, const RuntimeInfo& info,
            Backend* backend);

const char* describe(State state);

}  // namespace kcds::launcher::crash

#endif  // KCDS_LAUNCHER_CRASH_HPP
