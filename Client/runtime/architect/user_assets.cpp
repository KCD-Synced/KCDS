#include "user_assets.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../native_ui/native_ui.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"
#include "saves.hpp"

namespace kcds::runtime::architect::user_assets {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// What the folder may hold, at most.
constexpr std::uintmax_t kMostFileBytes = 64ull * 1024 * 1024;
constexpr std::uintmax_t kMostBytes = 512ull * 1024 * 1024;
constexpr std::size_t kMostFiles = 2000;
constexpr std::size_t kMostQuestions = 16;
// The kinds of file the game loads as a model, a material or a texture.
constexpr const char* kKinds[] = {".cgf", ".cgfm", ".mtl", ".dds", ".chr", ".skin", ".cdf"};

std::mutex queue_mutex;
std::vector<std::int64_t> questions;
std::vector<std::pair<std::int64_t, sdk::Value>> replies;
// What was mounted last (path, size and time of every file) and the packs' generation.
std::string mounted_signature;
int generation = 0;
std::vector<std::string> mounted_paths;

bool ends_with(const std::string& text, const std::string& end) {
  return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

// A texture's split parts: name.dds.1, name.dds.2 ...
bool texture_part(const std::string& path) {
  const std::size_t dot = path.rfind('.');
  if (dot == std::string::npos || dot + 1 >= path.size()) {
    return false;
  }
  const bool digits = std::all_of(path.begin() + std::ptrdiff_t(dot) + 1, path.end(),
                                  [](char c) { return std::isdigit(unsigned(c)) != 0; });
  return digits && ends_with(path.substr(0, dot), ".dds");
}

bool usable_kind(const std::string& path) {
  return texture_part(path) ||
         std::any_of(std::begin(kKinds), std::end(kKinds),
                     [&path](const char* kind) { return ends_with(path, kind); });
}

// A game path under <top>/kcds/<name>/, as a resource's streamed file must be.
bool own_place(const std::string& path) {
  const std::size_t top = path.find('/');
  if (top == std::string::npos || top == 0 || path.find("..") != std::string::npos) {
    return false;
  }
  const std::string rest = path.substr(top + 1);
  const std::size_t name = rest.find('/', 5);
  return rest.rfind("kcds/", 0) == 0 && name != std::string::npos && name > 5 &&
         name + 1 < rest.size();
}

struct Found {
  std::vector<std::pair<std::string, std::filesystem::path>> files;
  std::vector<std::string> refused;
  std::string signature;
};

Found look(const std::filesystem::path& folder) {
  Found found;
  std::error_code error;
  if (!std::filesystem::is_directory(folder, error)) {
    return found;
  }
  std::uintmax_t bytes = 0;
  auto entries = std::filesystem::recursive_directory_iterator(folder, error);
  for (auto it = entries; !error && it != std::filesystem::recursive_directory_iterator();
       it.increment(error)) {
    if (!it->is_regular_file(error)) {
      continue;
    }
    std::string path = std::filesystem::relative(it->path(), folder, error).generic_string();
    std::transform(path.begin(), path.end(), path.begin(),
                   [](char c) { return char(std::tolower(unsigned(c))); });
    const std::uintmax_t size = it->file_size(error);
    const bool fits =
        size <= kMostFileBytes && bytes + size <= kMostBytes && found.files.size() < kMostFiles;
    if (!own_place(path) || !usable_kind(path) || !fits) {
      found.refused.push_back(path);
      continue;
    }
    bytes += size;
    const auto time = it->last_write_time(error).time_since_epoch().count();
    found.signature += path + ":" + std::to_string(size) + ":" + std::to_string(time) + ";";
    found.files.emplace_back(path, it->path());
  }
  std::sort(found.files.begin(), found.files.end());
  return found;
}

bool read_file(const std::filesystem::path& file, std::vector<std::uint8_t>& out) {
  std::ifstream stream(file, std::ios::binary);
  if (!stream) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  return true;
}

std::string utf8(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

sdk::Value answer() {
  const std::filesystem::path folder = saves::architect_folder() / L"assets";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  Found found = look(folder);
  if (found.signature != mounted_signature) {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;
    for (const auto& [path, file] : found.files) {
      std::vector<std::uint8_t> data;
      if (read_file(file, data)) {
        files.emplace_back(path, std::move(data));
      }
    }
    generation += 1;
    mounted_paths = files.empty() ? std::vector<std::string>{}
                                  : native_ui::mount_local_files(std::to_string(generation), files);
    mounted_signature = found.signature;
  }
  sdk::Value::Array meshes;
  sdk::Value::Array materials;
  for (const std::string& path : mounted_paths) {
    if (ends_with(path, ".cgf")) {
      meshes.emplace_back(path);
    } else if (ends_with(path, ".mtl")) {
      materials.emplace_back(path.substr(0, path.size() - 4));
    }
  }
  sdk::Value::Array refused;
  for (const std::string& path : found.refused) {
    refused.emplace_back(path);
  }
  return sdk::Value(sdk::Value::Map{
      {"folder", sdk::Value(utf8(folder))},
      {"meshes", sdk::Value(std::move(meshes))},
      {"materials", sdk::Value(std::move(materials))},
      {"files", sdk::Value(double(mounted_paths.size()))},
      {"refused", sdk::Value(std::move(refused))},
  });
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = integration::script_field(message, "id");
  const sdk::Value* request = integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request ||
      integration::script_operation(*request) != "localUserAssets") {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (questions.size() < kMostQuestions) {
    questions.push_back(id->as_int());
  } else {
    replies.emplace_back(id->as_int(), sdk::Value());
  }
  return true;
}

void frame(bool in_world) {
  std::vector<std::int64_t> asked;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    asked.swap(questions);
  }
  if (asked.empty()) {
    return;
  }
  const sdk::Value value = in_world && local_mode::enabled() ? answer() : sdk::Value();
  std::lock_guard<std::mutex> lock(queue_mutex);
  for (const std::int64_t id : asked) {
    replies.emplace_back(id, value);
  }
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<std::pair<std::int64_t, sdk::Value>> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(replies);
  }
  for (auto& [id, value] : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(id)},
                                    {"value", std::move(value)}}));
  }
}

}  // namespace kcds::runtime::architect::user_assets
