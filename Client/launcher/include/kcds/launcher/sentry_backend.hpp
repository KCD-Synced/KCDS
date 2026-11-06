// The native crash-capture backend for the launcher and the game-client DLL:
// the pinned sentry-native SDK with its Crashpad backend (Windows only, built
// when KCDS_ENABLE_SENTRY is on).
//
// It is only ever started through crash::start(), which refuses to touch it
// unless the deployment configuration enables reporting with a valid DSN of
// our own; there is no built-in DSN or default endpoint.
//
// Privacy settings applied on every start:
//   - no default PII and no screenshots; the Crashpad report holds the
//     minidump, the SDK's event metadata and, only when SentryOptions::logs
//     names them, bounded log snapshots taken at crash time (crash_logs.hpp);
//   - no session tracking, performance tracing, structured logs or metrics;
//   - no breadcrumbs and no client reports;
//   - Crashpad's default minidump: thread stacks and module list, never full
//     process memory;
//   - pending reports live in a per-user directory (database_dir), never in
//     the install tree.
//
// Thread and loader-lock rules: start() spawns crashpad_handler.exe and must
// never run inside DllMain. In the game-client DLL it runs on the existing
// initialization thread, and the backend is intentionally never destroyed so
// no SDK shutdown runs under loader lock at process exit.
#ifndef KCDS_LAUNCHER_SENTRY_BACKEND_HPP
#define KCDS_LAUNCHER_SENTRY_BACKEND_HPP

#include <filesystem>
#include <string>
#include <vector>

#include <string>
#include <utility>
#include <vector>

#include "kcds/launcher/crash.hpp"
#include "kcds/launcher/crash_logs.hpp"

namespace kcds::launcher::crash {

struct SentryOptions {
  // Absolute path of crashpad_handler.exe (shipped next to the binaries).
  std::filesystem::path handler;
  // Keeps a crashing process alive until its report is uploaded. Off by
  // default: the report then waits in the database for the next start.
  bool wait_for_upload = false;
  // Logs whose tail is attached to a crash report (see crash_logs.hpp).
  std::vector<LogAttachment> logs;
  // Tags and a stack (one frame per line) the process adds about the moment of the crash, from the
  // crashing thread's EXCEPTION_POINTERS (Windows). Runs in the crashing process; may be null.
  using Annotate = void (*)(const void* exception_pointers,
                            std::vector<std::pair<std::string, std::string>>& tags,
                            std::string& stack);
  Annotate annotate = nullptr;
};

class SentryBackend : public Backend {
 public:
  explicit SentryBackend(SentryOptions options);
  ~SentryBackend() override;

  SentryBackend(const SentryBackend&) = delete;
  SentryBackend& operator=(const SentryBackend&) = delete;

  bool start(const Dsn& dsn, const Settings& settings, const RuntimeInfo& info) override;
  void stop() override;

 private:
  SentryOptions options_;
  bool started_ = false;
};

// The per-user report database for one component:
// %LOCALAPPDATA%\KCDS\sentry\<component>, created if missing. Returns an empty
// path when the folder is unavailable or component is not a tag.
std::filesystem::path default_database_dir(const std::string& component);

}  // namespace kcds::launcher::crash

#endif  // KCDS_LAUNCHER_SENTRY_BACKEND_HPP
