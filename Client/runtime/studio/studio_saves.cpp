// Winsock before Windows.h (see client_internal.hpp); then the shell's known folders.
#include "../core/client_internal.hpp"

#include <shlobj.h>

#include "studio_saves.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../core/log.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::studio::saves {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
namespace fs = std::filesystem;

constexpr std::size_t kLongestName = 48;
constexpr std::size_t kLargestSave = 60u * 1024u;

enum class Kind { List, SaveAs, Delete, Load };

struct Request {
  Kind kind = Kind::List;
  std::string name;
  std::string data;
};

std::mutex requests_mutex;
std::vector<Request> requests;

// Documents\KCDS\Asset Studio (empty when Windows does not say where Documents is).
fs::path saves_folder() {
  PWSTR documents = nullptr;
  fs::path folder;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents) {
    folder = fs::path(documents) / "KCDS" / "Asset Studio";
  }
  CoTaskMemFree(documents);
  return folder;
}

std::string text_field(const sdk::Value& message, const char* name) {
  const sdk::Value* field = integration::script_field(message, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

sdk::Value list_answer() {
  struct Save {
    std::string name;
    std::uintmax_t bytes = 0;
    std::int64_t time = 0;
  };
  std::vector<Save> found;
  const fs::path folder = saves_folder();
  std::error_code error;
  if (!folder.empty() && fs::is_directory(folder, error)) {
    for (const auto& entry : fs::directory_iterator(folder, error)) {
      const std::string name = entry.path().stem().string();
      if (entry.path().extension() != ".json" || !save_name(name)) {
        continue;
      }
      Save save;
      save.name = name;
      save.bytes = fs::file_size(entry.path(), error);
      const auto written = fs::last_write_time(entry.path(), error);
      const auto system = std::chrono::clock_cast<std::chrono::system_clock>(written);
      save.time =
          std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
      found.push_back(save);
    }
  }
  std::sort(found.begin(), found.end(),
            [](const Save& a, const Save& b) { return a.time > b.time; });
  sdk::Value::Array list;
  for (const Save& save : found) {
    list.push_back(sdk::Value(sdk::Value::Map{{"name", sdk::Value(save.name)},
                                              {"bytes", sdk::Value(std::int64_t(save.bytes))},
                                              {"time", sdk::Value(save.time)}}));
  }
  return sdk::Value(
      sdk::Value::Map{{"op", sdk::Value("studioSaves")}, {"saves", sdk::Value(list)}});
}

void write_save(const Request& request) {
  const fs::path folder = saves_folder();
  std::error_code error;
  const bool made = !folder.empty() && (fs::create_directories(folder, error) || !error);
  if (!made) {
    game_client::log("STUDIO save %s: no folder for the saves", request.name.c_str());
    return;
  }
  std::ofstream file(folder / (request.name + ".json"), std::ios::binary | std::ios::trunc);
  file.write(request.data.data(), std::streamsize(request.data.size()));
  game_client::log("STUDIO save %s: %s (%zu bytes)", request.name.c_str(),
                   file ? "written" : "could not be written", request.data.size());
}

sdk::Value load_answer(const Request& request) {
  std::string data;
  const fs::path folder = saves_folder();
  std::error_code error;
  const fs::path path = folder / (request.name + ".json");
  const auto size = folder.empty() ? 0 : fs::file_size(path, error);
  if (!error && size > 0 && size <= kLargestSave) {
    std::ifstream file(path, std::ios::binary);
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  return sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioSaveLoaded")},
                                    {"name", sdk::Value(request.name)},
                                    {"data", sdk::Value(data)}});
}

}  // namespace

bool save_name(const std::string& name) {
  if (name.empty() || name.size() > kLongestName || name.front() == ' ' || name.back() == ' ') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
           c == '-' || c == '_';
  });
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  Request request;
  if (operation == "studioSaveList") {
    request.kind = Kind::List;
  } else if (operation == "studioSaveAs") {
    request.kind = Kind::SaveAs;
    request.data = text_field(message, "data");
  } else if (operation == "studioSaveDelete") {
    request.kind = Kind::Delete;
  } else if (operation == "studioSaveLoad") {
    request.kind = Kind::Load;
  } else {
    return false;
  }
  if (request.kind != Kind::List) {
    request.name = text_field(message, "name");
    if (!save_name(request.name) || request.data.size() > kLargestSave) {
      return true;
    }
  }
  std::lock_guard<std::mutex> lock(requests_mutex);
  requests.push_back(request);
  return true;
}

void tick(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<Request> now;
  {
    std::lock_guard<std::mutex> lock(requests_mutex);
    now.swap(requests);
  }
  for (const Request& request : now) {
    if (request.kind == Kind::SaveAs) {
      write_save(request);
    } else if (request.kind == Kind::Delete) {
      std::error_code error;
      const fs::path folder = saves_folder();
      if (!folder.empty()) {
        fs::remove(folder / (request.name + ".json"), error);
      }
    } else if (request.kind == Kind::Load) {
      send(load_answer(request));
      continue;
    }
    send(list_answer());
  }
}

}  // namespace kcds::runtime::studio::saves
