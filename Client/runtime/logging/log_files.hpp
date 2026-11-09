// How the client's logs stay small on disk: one game's log is split in two parts of a fixed size,
// and the logs folder keeps only the newest logs, by count and by bytes together.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace kcds::game_client::log_files {

// A game's log grows to this size, then its lines so far move to its earlier part and the log
// starts over, so one game never holds more than twice this on disk.
constexpr std::uintmax_t kPartBytes = 32ull * 1024 * 1024;
// The client logs a logs folder keeps (earlier parts count as logs): the newest, up to this many
// files and this many bytes together.
constexpr std::size_t kKeptFiles = 20;
constexpr std::uintmax_t kKeptBytes = 256ull * 1024 * 1024;

// The earlier part of a log: client-<pid>.log has client-<pid>.old.log.
std::filesystem::path earlier_part(const std::filesystem::path& log);

// Removes the oldest client-*.log files of `folder` until at most `count` remain and together
// they hold at most `bytes`. The newest file always stays. Returns how many were removed.
std::size_t prune(const std::filesystem::path& folder, std::size_t count, std::uintmax_t bytes);

// Copies what `file` (open for writing at `path`) holds so far to the earlier part, replacing an
// older one, then empties the file and writes on from its start. The caller holds the file's lock.
// Returns false when the file could not be emptied; it then keeps growing.
bool start_over(std::FILE* file, const std::filesystem::path& path);

}  // namespace kcds::game_client::log_files
