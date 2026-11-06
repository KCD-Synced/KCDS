// Crash-report log attachments: the game-client log and snapshot paths, and
// the bounded crash-time tail copy (Windows). Nothing here crashes or sends.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <share.h>
#endif

#include "kcd2s/testkit.hpp"
#include "kcds/launcher/crash_logs.hpp"

namespace {

namespace fs = std::filesystem;
using kcds::launcher::crash::LogAttachment;

fs::path scratch_directory(const std::string& name) {
  fs::path directory = fs::temp_directory_path() / ("kcds-crash-logs-test-" + name);
  std::error_code error;
  fs::remove_all(directory, error);
  fs::create_directories(directory);
  return directory;
}

void write_text(const fs::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

}  // namespace

KCD2S_TEST(crash_logs, game_client_log_is_next_to_the_dll_and_snapshotted_in_the_database) {
  LogAttachment log =
      kcds::launcher::crash::game_client_log(fs::path("install"), 4242, fs::path("database"));
  KCD2S_CHECK(log.source == fs::path("install") / "client-4242.log");
  KCD2S_CHECK(log.snapshot == fs::path("database") / "logs" / "client-4242.log");
  KCD2S_CHECK_EQ(log.max_bytes, kcds::launcher::crash::kMaxLogAttachmentBytes);
}

#ifdef _WIN32
KCD2S_TEST(crash_logs, a_short_log_is_copied_whole) {
  fs::path directory = scratch_directory("short");
  fs::path source = directory / "client.log";
  fs::path snapshot = directory / "snapshot.log";
  write_text(source, "first line\nsecond line\n");

  KCD2S_CHECK(kcds::launcher::crash::copy_log_tail(source.c_str(), snapshot.c_str(), 1024));
  KCD2S_CHECK_EQ(read_text(snapshot), std::string("first line\nsecond line\n"));
}

KCD2S_TEST(crash_logs, a_long_log_keeps_whole_lines_from_its_end_with_a_marker) {
  fs::path directory = scratch_directory("long");
  fs::path source = directory / "client.log";
  fs::path snapshot = directory / "snapshot.log";

  // 3000 numbered lines of 22 bytes each, far above the 40000-byte limit,
  // so the copy crosses the 16 KiB internal buffer several times.
  std::string text;
  for (int index = 0; index < 3000; ++index) {
    char line[32];
    std::snprintf(line, sizeof(line), "line %06d payload..\n", index);
    text += line;
  }
  write_text(source, text);
  const std::uint64_t limit = 40000;

  KCD2S_CHECK(kcds::launcher::crash::copy_log_tail(source.c_str(), snapshot.c_str(), limit));
  std::string copied = read_text(snapshot);

  std::string marker =
      "[kcds: log truncated, last 40000 of " + std::to_string(text.size()) + " bytes]\r\n";
  KCD2S_CHECK(copied.rfind(marker, 0) == 0);
  std::string body = copied.substr(marker.size());
  KCD2S_CHECK(body.size() <= limit);
  // Cut at a line start, ending with the last line of the log.
  KCD2S_CHECK(body.rfind("line ", 0) == 0);
  std::string last_line = "line 002999 payload..\n";
  KCD2S_CHECK(body.size() >= last_line.size());
  KCD2S_CHECK_EQ(body.compare(body.size() - last_line.size(), last_line.size(), last_line), 0);
  KCD2S_CHECK_EQ(text.compare(text.size() - body.size(), body.size(), body), 0);
}

KCD2S_TEST(crash_logs, a_missing_log_leaves_no_snapshot) {
  fs::path directory = scratch_directory("missing");
  fs::path snapshot = directory / "snapshot.log";
  write_text(snapshot, "stale");

  fs::path source = directory / "absent.log";
  KCD2S_CHECK(!kcds::launcher::crash::copy_log_tail(source.c_str(), snapshot.c_str(), 1024));
  // A failed open of the source does not touch an existing snapshot path, and
  // the backend removes stale snapshots at start-up.
  KCD2S_CHECK_EQ(read_text(snapshot), std::string("stale"));
}

KCD2S_TEST(crash_logs, a_log_open_for_writing_is_still_copied) {
  fs::path directory = scratch_directory("open");
  fs::path source = directory / "client.log";
  fs::path snapshot = directory / "snapshot.log";

  // The DLL keeps its log open (shared) while it runs.
  FILE* log = _wfsopen(source.c_str(), L"w", _SH_DENYNO);
  KCD2S_CHECK(log != nullptr);
  std::fputs("still writing\n", log);
  std::fflush(log);

  KCD2S_CHECK(kcds::launcher::crash::copy_log_tail(source.c_str(), snapshot.c_str(), 1024));
  std::fclose(log);
  // Text mode, like the DLL log: the line ends in CR LF on disk.
  KCD2S_CHECK_EQ(read_text(snapshot), std::string("still writing\r\n"));
}
#endif
