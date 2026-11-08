// Log files attached to native crash reports.
//
// A crash report may carry the end of a log the crashing process wrote, so a
// crash can be read together with what happened just before it. The live log
// is never uploaded as it is: it can grow to many megabytes during a session.
// Instead the backend registers a snapshot path as the report attachment and,
// at crash time, copies at most max_bytes from the end of the live log into
// that snapshot (cut at a line start, with a marker line when truncated). The
// Crashpad handler then uploads the snapshot together with the minidump.
//
// Attachments only exist when crash reporting was started, which requires the
// deployment's DSN (crash::start); without a DSN no snapshot is written and
// nothing is sent. Snapshots live in the per-user report database, never in
// the install tree.
#ifndef KCDS_LAUNCHER_CRASH_LOGS_HPP
#define KCDS_LAUNCHER_CRASH_LOGS_HPP

#include <cstdint>
#include <filesystem>

namespace kcds::launcher::crash {

// Upper bound for one log attachment (the copied tail, without the marker).
inline constexpr std::uint64_t kMaxLogAttachmentBytes = 512 * 1024;

// Upper bound for a whole file attached to a crash report (newest_file).
// (The crash bridge keeps one such file of at most 24 MiB.)
inline constexpr std::uint64_t kMaxFileAttachmentBytes = 24ull * 1024 * 1024;

struct LogAttachment {
  std::filesystem::path source;    // the live log written by the process
  std::filesystem::path snapshot;  // the bounded copy that is uploaded
  std::uint64_t max_bytes = kMaxLogAttachmentBytes;
  // When set, `source` is a file pattern (a folder and a wildcard name): the newest matching file
  // written in the last `written_within_seconds` is copied whole, and nothing when it is larger
  // than max_bytes.
  bool newest_match = false;
  std::uint64_t written_within_seconds = 0;
};

// The game-client DLL log: client-<pid>.log in `folder` (the logs folder, or the DLL's
// directory when that could not be made; Client/runtime/core/log.cpp), with its snapshot
// <database_dir>/logs/client-<pid>.log. Two local clients share one report
// database; the process id keeps their snapshots apart.
LogAttachment game_client_log(const std::filesystem::path& folder, std::uint32_t pid,
                              const std::filesystem::path& database_dir);

// The launcher's own log (launcher_log.hpp), with its snapshot
// <database_dir>/logs/<log file name>.
LogAttachment launcher_log(const std::filesystem::path& log_path,
                           const std::filesystem::path& database_dir);

// The newest file matching `pattern` (a folder and a wildcard name, such as the game's
// AftermathDumps\*.nv-gpudmp) written in the last `within_seconds`, attached whole as
// <database_dir>/logs/<snapshot_name>: a file the process writes only as it fails.
LogAttachment newest_file(const std::filesystem::path& pattern,
                          const std::filesystem::path& snapshot_name,
                          const std::filesystem::path& database_dir, std::uint64_t within_seconds);

#ifdef _WIN32
// Copies the newest file matching pattern into snapshot (replacing it) when it was last written
// at most within_seconds ago and is at most max_bytes long. Returns false, leaving no snapshot,
// when there is no such file. Called from the crash handler: no heap memory, Win32 calls only.
bool copy_newest_file(const wchar_t* pattern, const wchar_t* snapshot, std::uint64_t max_bytes,
                      std::uint64_t within_seconds) noexcept;

// Copies at most max_bytes from the end of source into snapshot (replacing
// it). When the log is longer, the copy starts after the first line break of
// the kept range and is preceded by one marker line naming the kept and total
// sizes. Returns false, leaving no snapshot, when source cannot be read.
//
// Called from the crash handler: it allocates no heap memory, uses only
// Win32 file calls and one static buffer, and refuses (returns false) a
// concurrent second call instead of waiting.
bool copy_log_tail(const wchar_t* source, const wchar_t* snapshot,
                   std::uint64_t max_bytes) noexcept;
#endif

}  // namespace kcds::launcher::crash

#endif  // KCDS_LAUNCHER_CRASH_LOGS_HPP
