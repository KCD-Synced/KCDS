// Windows before the rest (GetModuleFileNameW).
#include "../core/client_internal.hpp"

#include "game_files.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/log.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "pack_index.hpp"
#include "studio_view.hpp"

namespace kcds::runtime::studio::game_files {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
namespace fs = std::filesystem;

constexpr unsigned long long kTailBytes = 65536 + 22;
constexpr int kPageSize = 24;
constexpr int kLargestPage = 200;

std::string lower(std::string text) {
  for (char& c : text) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

bool ends_with(const std::string& text, const char* end) {
  const std::size_t size = std::char_traits<char>::length(end);
  return text.size() > size && text.compare(text.size() - size, size, end) == 0;
}

struct Request {
  std::string kind;
  std::string query;
  int page = 0;
  int size = kPageSize;
};

std::mutex state_mutex;
std::vector<Request> requests;
std::vector<std::string> objects;
std::vector<std::string> clips;
std::atomic<bool> ready{false};
std::atomic<bool> reading{false};
// Searches asked while the packs were still read (engine thread).
std::vector<Request> waiting;

// The game's Data folder: two up from the game's exe's folder (Bin/Win64MasterMasterSteamPGO).
fs::path data_folder() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return fs::path(exe).parent_path().parent_path().parent_path() / "Data";
}

// A pack's file names (none when it is not a zip).
std::vector<std::string> pack_names(const fs::path& pack) {
  std::ifstream file(pack, std::ios::binary);
  if (!file) {
    return {};
  }
  file.seekg(0, std::ios::end);
  const auto size = static_cast<unsigned long long>(file.tellg());
  const unsigned long long tail_start = size > kTailBytes ? size - kTailBytes : 0;
  std::vector<unsigned char> tail(std::size_t(size - tail_start));
  file.seekg(std::streamoff(tail_start));
  file.read(reinterpret_cast<char*>(tail.data()), std::streamsize(tail.size()));
  unsigned long long offset = 0;
  unsigned long long length = 0;
  if (!file || !pack_index::find_directory(tail, tail_start, offset, length) ||
      offset + length > size) {
    return {};
  }
  std::vector<unsigned char> directory(static_cast<std::size_t>(length));
  file.seekg(std::streamoff(offset));
  file.read(reinterpret_cast<char*>(directory.data()), std::streamsize(directory.size()));
  return file ? pack_index::directory_names(directory) : std::vector<std::string>();
}

// Reads every pack's names into the lists (on a thread of its own).
void read_packs() {
  std::vector<std::string> found_objects;
  std::vector<std::string> found_clips;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(data_folder(), error)) {
    if (entry.path().extension() != ".pak") {
      continue;
    }
    for (const std::string& name : pack_names(entry.path())) {
      const std::string low = lower(name);
      if (low.rfind("objects/", 0) == 0 && ends_with(low, ".cgf") &&
          low.find("_lod") == std::string::npos) {
        found_objects.push_back(name);
      } else if (low.rfind("animations/", 0) == 0 && ends_with(low, ".caf")) {
        found_clips.push_back(name);
      }
    }
  }
  for (auto* list : {&found_objects, &found_clips}) {
    std::sort(list->begin(), list->end(),
              [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    list->erase(std::unique(list->begin(), list->end()), list->end());
  }
  std::size_t object_count = 0;
  std::size_t clip_count = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    objects = std::move(found_objects);
    clips = std::move(found_clips);
    object_count = objects.size();
    clip_count = clips.size();
  }
  game_client::log("STUDIO game files: %zu objects, %zu clips", object_count, clip_count);
  ready.store(true);
}

sdk::Value not_ready(const Request& request) {
  return sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioGameFilesPage")},
                                    {"kind", sdk::Value(request.kind)},
                                    {"query", sdk::Value(request.query)},
                                    {"size", sdk::Value(std::int64_t(request.size))},
                                    {"ready", sdk::Value(false)}});
}

sdk::Value page_of(const Request& request) {
  std::vector<std::string> found;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    found = pack_index::matching(request.kind == "clips" ? clips : objects, request.query);
  }
  const int total = int(found.size());
  const int size = request.size;
  const int pages = std::max(1, (total + size - 1) / size);
  const int page = std::min(request.page, pages - 1);
  sdk::Value::Array paths;
  for (int index = page * size; index < std::min(total, (page + 1) * size); ++index) {
    paths.push_back(sdk::Value(found[std::size_t(index)]));
  }
  return sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioGameFilesPage")},
                                    {"kind", sdk::Value(request.kind)},
                                    {"query", sdk::Value(request.query)},
                                    {"page", sdk::Value(std::int64_t(page))},
                                    {"size", sdk::Value(std::int64_t(size))},
                                    {"pages", sdk::Value(std::int64_t(pages))},
                                    {"total", sdk::Value(std::int64_t(total))},
                                    {"ready", sdk::Value(true)},
                                    {"paths", sdk::Value(std::move(paths))}});
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "studioGameFiles") {
    return false;
  }
  Request request;
  const sdk::Value* kind = integration::script_field(message, "kind");
  request.kind =
      kind && kind->tag() == sdk::Tag::Str && kind->as_str() == "clips" ? "clips" : "objects";
  const sdk::Value* query = integration::script_field(message, "query");
  request.query = query && query->tag() == sdk::Tag::Str ? query->as_str().substr(0, 80) : "";
  const sdk::Value* page = integration::script_field(message, "page");
  double number = 0;
  if (page && view::number_of(*page, number)) {
    request.page = std::max(0, int(number));
  }
  const sdk::Value* size = integration::script_field(message, "size");
  if (size && view::number_of(*size, number)) {
    request.size = std::clamp(int(number), 1, kLargestPage);
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  requests.push_back(request);
  return true;
}

void tick(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<Request> now;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    now.swap(requests);
  }
  if (!ready.load()) {
    if (now.empty()) {
      return;
    }
    if (!reading.exchange(true)) {
      std::thread(read_packs).detach();
    }
    // Told once, answered once the packs are read.
    for (const Request& request : now) {
      send(not_ready(request));
      waiting.push_back(request);
    }
    return;
  }
  if (!waiting.empty()) {
    now.insert(now.begin(), waiting.begin(), waiting.end());
    waiting.clear();
  }
  for (const Request& request : now) {
    send(page_of(request));
  }
}

}  // namespace kcds::runtime::studio::game_files
