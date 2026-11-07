#include "asset_files.hpp"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../files/base64.hpp"
#include "../native_ui/native_ui.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"
#include "model_slots.hpp"
#include "saves.hpp"

namespace kcds::runtime::architect::asset_files {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// The most a read material's text and a written file may hold.
constexpr std::size_t kMostTextBytes = 1024 * 1024;
constexpr std::size_t kMostFileBytes = 32 * 1024 * 1024;
constexpr std::size_t kMostWrites = 8;

std::mutex mutex;
std::vector<std::pair<std::int64_t, sdk::Value>> replies;
// Files coming in parts: the bytes so far and the part expected next, by path.
struct Collecting {
  std::vector<std::uint8_t> bytes;
  double next = 0;
};
std::map<std::string, Collecting> collecting;

std::string text(const sdk::Value& message, const char* name) {
  const sdk::Value* field = integration::script_field(message, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

double number(const sdk::Value& message, const char* name, double fallback) {
  const sdk::Value* field = integration::script_field(message, name);
  if (field && field->tag() == sdk::Tag::Double) {
    return field->as_double();
  }
  if (field && field->tag() == sdk::Tag::Int) {
    return double(field->as_int());
  }
  return fallback;
}

std::string lower(std::string path) {
  for (char& c : path) {
    c = c == '\\' ? '/' : char(std::tolower(unsigned(c)));
  }
  return path;
}

std::filesystem::path assets_folder() {
  return saves::architect_folder() / L"assets";
}

bool own_path(const std::string& path) {
  return path.rfind("objects/kcds/", 0) == 0 && path.find("..") == std::string::npos;
}

// A file's bytes: the author's own from the assets folder, the game's from its file system.
bool read_any(const std::string& path, std::vector<std::uint8_t>& out) {
  if (own_path(path)) {
    std::ifstream stream(assets_folder() / std::filesystem::u8path(path), std::ios::binary);
    if (!stream) {
      return false;
    }
    out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
  }
  if (path.find("..") != std::string::npos) {
    return false;
  }
  return native_ui::read_game_data(path, out);
}

sdk::Value slots_of(const std::string& mesh) {
  std::vector<std::uint8_t> bytes;
  std::string material;
  std::vector<std::string> slots;
  if (!read_any(mesh, bytes) || !model_slots::read(bytes, material, slots)) {
    return sdk::Value();
  }
  sdk::Value::Array names;
  for (std::string& slot : slots) {
    names.emplace_back(std::move(slot));
  }
  return sdk::Value(
      sdk::Value::Map{{"material", sdk::Value(material)}, {"slots", sdk::Value(std::move(names))}});
}

sdk::Value text_of(const std::string& path) {
  const std::string wanted = lower(path);
  if (wanted.size() < 5 || wanted.compare(wanted.size() - 4, 4, ".mtl") != 0) {
    return sdk::Value();
  }
  std::vector<std::uint8_t> bytes;
  if (!read_any(wanted, bytes) || bytes.size() > kMostTextBytes) {
    return sdk::Value();
  }
  return sdk::Value(std::string(bytes.begin(), bytes.end()));
}

void write(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const std::filesystem::path file = assets_folder() / std::filesystem::u8path(path);
  std::error_code error;
  std::filesystem::create_directories(file.parent_path(), error);
  std::ofstream stream(file, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

void answer(std::int64_t id, sdk::Value value) {
  std::lock_guard<std::mutex> lock(mutex);
  replies.emplace_back(id, std::move(value));
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "architectAssetWrite") {
    const std::string path = lower(text(message, "path"));
    if (!local_mode::enabled() || !model_slots::writable(path)) {
      return true;
    }
    std::vector<std::uint8_t> data;
    if (!files::decode_base64(text(message, "data"), data)) {
      return true;
    }
    const double parts = number(message, "parts", 1);
    const double part = number(message, "part", 0);
    std::vector<std::uint8_t> whole;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (part == 0) {
        if (collecting.size() >= kMostWrites && collecting.count(path) == 0) {
          return true;
        }
        collecting[path] = Collecting{};
      }
      auto found = collecting.find(path);
      if (found == collecting.end() || part != found->second.next ||
          found->second.bytes.size() + data.size() > kMostFileBytes) {
        collecting.erase(path);
        return true;
      }
      found->second.bytes.insert(found->second.bytes.end(), data.begin(), data.end());
      found->second.next += 1;
      if (found->second.next < parts) {
        return true;
      }
      whole = std::move(found->second.bytes);
      collecting.erase(found);
    }
    write(path, whole);
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
  if (asked == "localModelSlots") {
    answer(id->as_int(),
           local_mode::enabled() ? slots_of(lower(text(*request, "mesh"))) : sdk::Value());
    return true;
  }
  if (asked == "localAssetText") {
    answer(id->as_int(), local_mode::enabled() ? text_of(text(*request, "path")) : sdk::Value());
    return true;
  }
  return false;
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<std::pair<std::int64_t, sdk::Value>> taken;
  {
    std::lock_guard<std::mutex> lock(mutex);
    taken.swap(replies);
  }
  for (auto& [id, value] : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(id)},
                                    {"value", std::move(value)}}));
  }
}

}  // namespace kcds::runtime::architect::asset_files
