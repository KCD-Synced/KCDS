#include "log_files.hpp"

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace kcds::game_client::log_files {
namespace {

struct Found {
  std::filesystem::file_time_type written;
  std::uintmax_t size = 0;
  std::filesystem::path path;
};

bool is_client_log(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  return name.rfind("client-", 0) == 0 && path.extension() == ".log";
}

// Empties an open file through its descriptor.
bool truncate_file(std::FILE* file) {
#ifdef _WIN32
  return _chsize_s(_fileno(file), 0) == 0;
#else
  return ftruncate(fileno(file), 0) == 0;
#endif
}

}  // namespace

std::filesystem::path earlier_part(const std::filesystem::path& log) {
  std::filesystem::path part = log;
  part.replace_extension(".old.log");
  return part;
}

std::size_t prune(const std::filesystem::path& folder, std::size_t count, std::uintmax_t bytes) {
  std::error_code error;
  std::vector<Found> found;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (!is_client_log(entry.path())) {
      continue;
    }
    std::error_code entry_error;
    Found file;
    file.written = entry.last_write_time(entry_error);
    file.size = entry.file_size(entry_error);
    file.path = entry.path();
    found.push_back(std::move(file));
  }
  // Newest first: the files kept are a run from the front.
  std::sort(found.begin(), found.end(),
            [](const Found& a, const Found& b) { return a.written > b.written; });
  std::size_t removed = 0;
  std::uintmax_t kept_bytes = 0;
  for (std::size_t index = 0; index < found.size(); ++index) {
    kept_bytes += found[index].size;
    const bool over = index >= count || kept_bytes > bytes;
    if (index == 0 || !over) {
      continue;
    }
    std::error_code remove_error;
    if (std::filesystem::remove(found[index].path, remove_error)) {
      ++removed;
    }
  }
  return removed;
}

bool start_over(std::FILE* file, const std::filesystem::path& path) {
  std::fflush(file);
  std::error_code error;
  std::filesystem::copy_file(path, earlier_part(path),
                             std::filesystem::copy_options::overwrite_existing, error);
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    return false;
  }
  return truncate_file(file);
}

}  // namespace kcds::game_client::log_files
