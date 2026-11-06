#include "saves.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../maps/custom_maps.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>

namespace kcds::runtime::architect::saves {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr std::size_t kMostBytes = 4 * 1024 * 1024;
constexpr std::size_t kLongestName = 48;
constexpr std::size_t kMostSaves = 200;
// A save read back is answered in pieces of this many bytes (a message holds 64 KiB).
constexpr std::size_t kReadPiece = 40000;
// An export (the ground's mesh, the scene, the import script) may be larger than a save.
constexpr std::size_t kMostExportBytes = 64 * 1024 * 1024;

std::mutex file_mutex;
std::mutex reply_mutex;
std::vector<std::pair<std::int64_t, sdk::Value>> replies;

}  // namespace

std::filesystem::path architect_folder() {
  PWSTR documents = nullptr;
  std::filesystem::path place;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
    place = std::filesystem::path(documents) / L"KCDS" / L"World Architect";
  }
  CoTaskMemFree(documents);
  return place;
}

namespace {

std::filesystem::path folder() {
  return architect_folder();
}

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

// A name the file system may have: letters, digits, spaces, '-' and '_', not blank.
bool usable(const std::string& name) {
  if (name.empty() || name.size() > kLongestName || name.front() == ' ' || name.back() == ' ') {
    return false;
  }
  for (const char c : name) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != ' ' && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

std::filesystem::path file_of(const std::string& name) {
  return folder() / std::filesystem::path(name + ".json");
}

void write(const std::string& name, const std::string& data) {
  const std::filesystem::path place = folder();
  if (place.empty() || !usable(name) || data.size() > kMostBytes) {
    return;
  }
  std::lock_guard<std::mutex> lock(file_mutex);
  std::error_code error;
  std::filesystem::create_directories(place, error);
  const std::filesystem::path file = file_of(name);
  std::filesystem::path next = file;
  next += L".next";
  {
    std::ofstream out(next, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    if (!out) {
      return;
    }
  }
  std::filesystem::rename(next, file, error);
}

// An exported file's name: letters, digits, '-', '_' and one '.', with a kind Blender or a text
// editor opens (.obj, .mtl, .json, .py, .txt), or a server runs (.js, Export for server).
bool usable_file(const std::string& name) {
  constexpr const char* kKinds[] = {".obj", ".mtl", ".json", ".py", ".txt", ".js"};
  const std::size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || !usable(name.substr(0, dot)) ||
      name.substr(0, dot).find(' ') != std::string::npos) {
    return false;
  }
  const std::string kind = name.substr(dot);
  return std::any_of(std::begin(kKinds), std::end(kKinds),
                     [&kind](const char* known) { return kind == known; });
}

// Writes an export (Export for Blender, editor.cjs) into exports\<folder>\, replacing one so
// named; its folder's path, or null when nothing could be written.
sdk::Value write_export(const sdk::Value& request) {
  const std::string name = text(request, "folder");
  const sdk::Value* files = integration::script_field(request, "files");
  const std::filesystem::path place = folder() / L"exports" / std::filesystem::path(name);
  if (folder().empty() || !usable(name) || !files || files->tag() != sdk::Tag::Array) {
    return sdk::Value();
  }
  std::size_t bytes = 0;
  for (const sdk::Value& file : files->as_array()) {
    bytes += text(file, "text").size();
    if (!usable_file(text(file, "name")) || bytes > kMostExportBytes) {
      return sdk::Value();
    }
  }
  std::lock_guard<std::mutex> lock(file_mutex);
  std::error_code error;
  std::filesystem::create_directories(place, error);
  for (const sdk::Value& file : files->as_array()) {
    const std::string data = text(file, "text");
    // A file larger than a message comes in pieces: the first replaces it, the others append.
    const sdk::Value* append = integration::script_field(file, "append");
    const bool more = append && append->tag() == sdk::Tag::Bool && append->as_bool();
    std::ofstream out(place / std::filesystem::path(text(file, "name")),
                      std::ios::binary | (more ? std::ios::app : std::ios::trunc));
    out.write(data.data(), std::streamsize(data.size()));
    if (!out) {
      return sdk::Value();
    }
  }
  // A custom map goes with the export; one that cannot be copied fails it.
  const std::string map = text(request, "map");
  if (!map.empty() && !maps::custom::copy_to(map, place / L"maps")) {
    return sdk::Value();
  }
  const auto path = place.u8string();
  return sdk::Value(std::string(path.begin(), path.end()));
}

void remove(const std::string& name) {
  if (!usable(name)) {
    return;
  }
  std::lock_guard<std::mutex> lock(file_mutex);
  std::error_code error;
  std::filesystem::remove(file_of(name), error);
}

sdk::Value list() {
  sdk::Value::Array found;
  const std::filesystem::path place = folder();
  std::lock_guard<std::mutex> lock(file_mutex);
  std::error_code error;
  if (place.empty() || !std::filesystem::is_directory(place, error)) {
    return sdk::Value(std::move(found));
  }
  for (const auto& entry : std::filesystem::directory_iterator(place, error)) {
    if (found.size() >= kMostSaves || !entry.is_regular_file(error)) {
      continue;
    }
    const std::filesystem::path path = entry.path();
    if (path.extension() != L".json") {
      continue;
    }
    const std::string name = path.stem().string();
    if (!usable(name)) {
      continue;
    }
    const auto written = std::filesystem::last_write_time(path, error);
    const auto since = std::chrono::duration_cast<std::chrono::seconds>(
        written.time_since_epoch() -
        std::filesystem::file_time_type::clock::now().time_since_epoch() +
        std::chrono::system_clock::now().time_since_epoch());
    found.push_back(sdk::Value(sdk::Value::Map{
        {"name", sdk::Value(name)},
        {"bytes", sdk::Value(double(entry.file_size(error)))},
        {"time", sdk::Value(double(since.count()))},
    }));
  }
  return sdk::Value(std::move(found));
}

// A save's text from `offset`, at most kReadPiece bytes (cut between whole UTF-8 characters: a
// message holds 64 KiB): { text, next }, next -1 at the end; null when there is no such save.
sdk::Value read(const std::string& name, std::size_t offset) {
  if (!usable(name)) {
    return sdk::Value();
  }
  std::lock_guard<std::mutex> lock(file_mutex);
  std::ifstream in(file_of(name), std::ios::binary);
  if (!in) {
    return sdk::Value();
  }
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() > kMostBytes || offset > data.size()) {
    return sdk::Value();
  }
  std::size_t end = (std::min)(data.size(), offset + kReadPiece);
  while (end < data.size() && end > offset && (std::uint8_t(data[end]) & 0xC0) == 0x80) {
    --end;
  }
  const double next = end < data.size() ? double(end) : -1.0;
  return sdk::Value(sdk::Value::Map{{"text", sdk::Value(data.substr(offset, end - offset))},
                                    {"next", sdk::Value(next)}});
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

// A save coming in parts (a message holds 64 KiB): its text so far and the part expected next.
struct Collecting {
  std::string text;
  std::size_t next = 0;
};
std::map<std::string, Collecting> collected;

void answer(std::int64_t id, sdk::Value value) {
  std::lock_guard<std::mutex> lock(reply_mutex);
  replies.emplace_back(id, std::move(value));
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "architectSaveAs") {
    const std::string name = text(message, "name");
    std::string data = text(message, "data");
    const double parts = number(message, "parts", 1);
    if (local_mode::enabled() && parts > 1) {
      const double part = number(message, "part", 0);
      std::lock_guard<std::mutex> lock(reply_mutex);
      Collecting& collecting = collected[name];
      if (part == 0) {
        collecting = Collecting{};
      }
      if (part != double(collecting.next) || collecting.text.size() + data.size() > kMostBytes) {
        collected.erase(name);
        return true;
      }
      collecting.text += data;
      collecting.next += 1;
      if (double(collecting.next) < parts) {
        return true;
      }
      data = std::move(collecting.text);
      collected.erase(name);
    }
    if (local_mode::enabled()) {
      write(name, data);
    }
    return true;
  }
  if (operation == "architectSaveDelete") {
    if (local_mode::enabled()) {
      remove(text(message, "name"));
    }
    return true;
  }
  if (operation != "syncRequest") {
    return false;
  }
  const sdk::Value* id = integration::script_field(message, "id");
  const sdk::Value* request = integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request) {
    return false;
  }
  const std::string asked = integration::script_operation(*request);
  if (asked == "localArchitectSaves") {
    answer(id->as_int(), local_mode::enabled() ? list() : sdk::Value());
    return true;
  }
  // The custom maps the editor can open (World Architect's map choice and the AI's switch_map).
  if (asked == "localArchitectMaps") {
    sdk::Value::Array maps;
    if (local_mode::enabled()) {
      for (const std::string& name : kcds::runtime::maps::custom::names()) {
        maps.push_back(sdk::Value(name));
      }
    }
    answer(id->as_int(), sdk::Value(std::move(maps)));
    return true;
  }
  if (asked == "localArchitectExport") {
    answer(id->as_int(), local_mode::enabled() ? write_export(*request) : sdk::Value());
    return true;
  }
  if (asked == "localArchitectSaveRead") {
    const double offset = number(*request, "offset", 0);
    answer(id->as_int(), local_mode::enabled() && offset >= 0
                             ? read(text(*request, "name"), std::size_t(offset))
                             : sdk::Value());
    return true;
  }
  return false;
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<std::pair<std::int64_t, sdk::Value>> taken;
  {
    std::lock_guard<std::mutex> lock(reply_mutex);
    taken.swap(replies);
  }
  for (auto& [id, value] : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(id)},
                                    {"value", std::move(value)}}));
  }
}

}  // namespace kcds::runtime::architect::saves
