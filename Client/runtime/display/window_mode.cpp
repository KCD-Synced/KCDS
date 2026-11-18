#include "window_mode.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include "../branding/runtime_branding.hpp"
#include "../native_ui/native_ui.hpp"
#include "MinHook.h"
#include "kcd2s/sdk/value.hpp"
#include "window_mode_memory.hpp"

namespace kcds::runtime::display::window_mode {
namespace {

namespace sdk = kcd2s::sdk;

using GraphicsPage = std::int64_t(__fastcall*)(void* settings);
using WriteIntAttribute = char(__fastcall*)(void* attributes, const char* name, const int* value);

GraphicsPage original_graphics_page = nullptr;
WriteIntAttribute original_write_int_attribute = nullptr;
Logger log_line = nullptr;

constexpr const char* kWindowTypeCvar = "r_WindowType";
// The profile attribute the game saves r_WindowType in.
constexpr const char* kWindowTypeAttribute = "res_fs";
// The game's Window mode row (Libs/Tables/ui/menu_buttons.xml) and its list container.
constexpr const char* kWindowModeRow = "WindowMode";
constexpr std::int64_t kListContainer = 0;
constexpr const char* kMenuElement = "Menu";
constexpr const char* kBorderlessLabel = "Borderless window";

// The memory is read once, on the engine thread, and changed there by the profile save.
std::mutex memory_mutex;
bool memory_read = false;
WindowModeMemory memory;

std::filesystem::path memory_path() {
  const char* base = std::getenv("LOCALAPPDATA");
  return std::filesystem::path(base ? base : ".") / "KCDS" / "window-mode.json";
}

// Under memory_mutex.
void read_memory_once() {
  if (memory_read) {
    return;
  }
  memory_read = true;
  std::ifstream file(memory_path());
  if (!file) {
    return;
  }
  std::stringstream text;
  text << file.rdbuf();
  memory = from_text(text.str());
}

// Under memory_mutex.
void write_memory() {
  std::error_code ignored;
  std::filesystem::create_directories(memory_path().parent_path(), ignored);
  std::ofstream file(memory_path(), std::ios::trunc);
  if (!file) {
    if (log_line) {
      log_line("DISPLAY window mode not saved: %s cannot be written",
               memory_path().string().c_str());
    }
    return;
  }
  file << to_text(memory);
}

// r_WindowType as the engine has it; -1 before the console is there.
int current_window_type() {
  const std::string text = kcds::runtime::branding::cvar_value(kWindowTypeCvar);
  if (text.empty()) {
    return -1;
  }
  return std::atoi(text.c_str());
}

// Branding's startup line: the borderless window when the player chose it, set before the
// renderer opens the window.
bool startup_window_type(std::string& name, std::string& value) {
  const int window_type = current_window_type();
  if (window_type < 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(memory_mutex);
  read_memory_once();
  const GameStart start = on_game_start(window_type, memory);
  memory = start.memory;
  if (log_line) {
    log_line("DISPLAY window type %d from the game's profile, starting in %d", window_type,
             start.window_type);
  }
  if (start.window_type == window_type) {
    return false;
  }
  name = kWindowTypeCvar;
  value = std::to_string(start.window_type);
  return true;
}

sdk::Value call(const char* function, sdk::Value::Array args) {
  return kcds::runtime::native_ui::request(
      sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("elementCall"))},
                                 {"name", sdk::Value(std::string(kMenuElement))},
                                 {"instance", sdk::Value(std::int64_t{0})},
                                 {"fn", sdk::Value(std::string(function))},
                                 {"args", sdk::Value(std::move(args))}}));
}

// The Graphics page is built with the game's two window modes; the borderless window joins them,
// and is the row's choice while it is in effect (the game found no choice of its own for it).
std::int64_t __fastcall hooked_graphics_page(void* settings) {
  const std::int64_t result = original_graphics_page(settings);
  call("AddChoiceOption",
       sdk::Value::Array{sdk::Value(std::int64_t{kBorderlessWindow}),
                         sdk::Value(std::string(kWindowModeRow)), sdk::Value(kListContainer),
                         sdk::Value(std::string(kBorderlessLabel)), sdk::Value(std::string()),
                         sdk::Value(false)});
  if (current_window_type() == kBorderlessWindow) {
    call("SetChoice",
         sdk::Value::Array{sdk::Value(std::string(kWindowModeRow)), sdk::Value(kListContainer),
                           sdk::Value(std::int64_t{kBorderlessWindow})});
  }
  return result;
}

// The profile save writes the window type: a mode of the game's own menu goes in as it is and is
// remembered; the borderless window leaves the last such mode in the profile.
char __fastcall hooked_write_int_attribute(void* attributes, const char* name, const int* value) {
  if (!name || !value || std::strcmp(name, kWindowTypeAttribute) != 0) {
    return original_write_int_attribute(attributes, name, value);
  }
  int saved = *value;
  {
    std::lock_guard<std::mutex> lock(memory_mutex);
    read_memory_once();
    const ProfileSave save = on_profile_save(*value, memory);
    saved = save.saved;
    if (!(save.memory == memory)) {
      memory = save.memory;
      write_memory();
      if (log_line) {
        log_line("DISPLAY window mode remembered: borderless %d, the game's own %d",
                 memory.borderless ? 1 : 0, memory.vanilla);
      }
    }
  }
  if (saved != *value && log_line) {
    log_line("DISPLAY profile keeps window type %d instead of %d", saved, *value);
  }
  return original_write_int_attribute(attributes, name, &saved);
}

bool hook(void* target, void* detour, void** original) {
  return MH_CreateHook(target, detour, original) == MH_OK;
}

}  // namespace

void start(Logger log) {
  log_line = log;
  if (!kcds::runtime::branding::add_startup_setting_source(startup_window_type) && log_line) {
    log_line("DISPLAY window mode not among the startup settings: no room for it");
  }
}

bool install(const Natives& natives) {
  if (!natives.graphics_page || !natives.write_int_attribute) {
    return false;
  }
  const bool created =
      hook(natives.graphics_page, reinterpret_cast<void*>(hooked_graphics_page),
           reinterpret_cast<void**>(&original_graphics_page)) &&
      hook(natives.write_int_attribute, reinterpret_cast<void*>(hooked_write_int_attribute),
           reinterpret_cast<void**>(&original_write_int_attribute));
  if (!created) {
    MH_RemoveHook(natives.graphics_page);
    MH_RemoveHook(natives.write_int_attribute);
    return false;
  }
  MH_EnableHook(natives.graphics_page);
  MH_EnableHook(natives.write_int_attribute);
  return true;
}

}  // namespace kcds::runtime::display::window_mode
