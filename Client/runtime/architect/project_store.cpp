#include "project_store.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>

#include "../studio/studio_scene.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "kcd2s/integration/world_config.hpp"
#include "local_mode.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace kcds::runtime::architect::projects {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr std::size_t kMaxProject = 4 * 1024 * 1024;

std::mutex file_mutex;

std::filesystem::path folder() {
  wchar_t local[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return std::filesystem::path(local) / L"KCDS" / L"architect" / L"projects";
}

// Only the maps the local mode loads name a project file.
bool known_map(const std::string& map) {
  // The Asset/Animation Studio keeps its project here too, under its level's name.
  return map == "trosecko" || map == "kutnohorsko" || map == "klaster" || map == studio::kLevel ||
         integration::custom_map_name(map);
}

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

double number(const sdk::Value& value, const char* name, double fallback) {
  const sdk::Value* field = integration::script_field(value, name);
  if (field && field->tag() == sdk::Tag::Double) {
    return field->as_double();
  }
  if (field && field->tag() == sdk::Tag::Int) {
    return double(field->as_int());
  }
  return fallback;
}

// A project coming in parts: the text so far and the part expected next, by map.
struct Collecting {
  std::string text;
  std::size_t next = 0;
};
std::map<std::string, Collecting> collected;

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "architectSave") {
    return false;
  }
  const std::string map = text(message, "map");
  std::string data = text(message, "data");
  if (!local_mode::enabled() || !known_map(map) || data.size() > kMaxProject) {
    return true;
  }
  // A project larger than a message (64 KiB) comes in parts, in order: written once whole (a save
  // over 64 KiB was dropped and the work since lost).
  const double parts = number(message, "parts", 1);
  if (parts > 1) {
    const double part = number(message, "part", 0);
    std::lock_guard<std::mutex> lock(file_mutex);
    Collecting& collecting = collected[map];
    if (part == 0) {
      collecting = Collecting{};
    }
    if (part != double(collecting.next) || collecting.text.size() + data.size() > kMaxProject) {
      collected.erase(map);
      return true;
    }
    collecting.text += data;
    collecting.next += 1;
    if (double(collecting.next) < parts) {
      return true;
    }
    data = std::move(collecting.text);
    collected.erase(map);
  }
  const std::filesystem::path place = folder();
  if (place.empty()) {
    return true;
  }
  std::lock_guard<std::mutex> lock(file_mutex);
  std::error_code error;
  std::filesystem::create_directories(place, error);
  // Written beside and then put in place, so a failed write leaves the last project whole.
  const std::filesystem::path file = place / (map + ".json");
  const std::filesystem::path next = place / (map + ".json.next");
  {
    std::ofstream out(next, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    if (!out) {
      return true;
    }
  }
  std::filesystem::rename(next, file, error);
  return true;
}

std::string load(const std::string& map) {
  if (!known_map(map)) {
    return {};
  }
  const std::filesystem::path place = folder();
  if (place.empty()) {
    return {};
  }
  std::lock_guard<std::mutex> lock(file_mutex);
  std::ifstream in(place / (map + ".json"), std::ios::binary);
  if (!in) {
    return {};
  }
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return data.size() <= kMaxProject ? data : std::string();
}

}  // namespace kcds::runtime::architect::projects
