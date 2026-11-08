// Crash-report log attachments: where the logs and their snapshots live.
// See kcds/launcher/crash_logs.hpp.
#include "kcds/launcher/crash_logs.hpp"

#include <string>

namespace kcds::launcher::crash {

LogAttachment game_client_log(const std::filesystem::path& folder, std::uint32_t pid,
                              const std::filesystem::path& database_dir) {
  std::string name = "client-" + std::to_string(pid) + ".log";

  LogAttachment attachment;
  attachment.source = folder / name;
  attachment.snapshot = database_dir / "logs" / name;
  return attachment;
}

LogAttachment launcher_log(const std::filesystem::path& log_path,
                           const std::filesystem::path& database_dir) {
  LogAttachment attachment;
  attachment.source = log_path;
  attachment.snapshot = database_dir / "logs" / log_path.filename();
  return attachment;
}

LogAttachment newest_file(const std::filesystem::path& pattern,
                          const std::filesystem::path& snapshot_name,
                          const std::filesystem::path& database_dir, std::uint64_t within_seconds) {
  LogAttachment attachment;
  attachment.source = pattern;
  attachment.snapshot = database_dir / "logs" / snapshot_name;
  attachment.max_bytes = kMaxFileAttachmentBytes;
  attachment.newest_match = true;
  attachment.written_within_seconds = within_seconds;
  return attachment;
}

}  // namespace kcds::launcher::crash
