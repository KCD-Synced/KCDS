// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "local_mode.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cwchar>
#include <string>
#include <vector>

#include "../chat/chat_overlay.hpp"
#include "../core/log.hpp"
#include "../studio/studio_rope.hpp"
#include "../studio/studio_scene.hpp"
#include "editor_pipe.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "editor_overlay.hpp"
#include "player_body.hpp"
#include "project_store.hpp"
#include "../terrain/terrain_ops.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace kcds::runtime::architect::local_mode {
namespace {

namespace integration = kcd2s::integration;
namespace sdk = kcd2s::sdk;

// The seconds after the world is ready before the editor opens: none, so it is built (hidden)
// while the game's loading screen is still up and the player lands in it.
constexpr double kOpenDelaySeconds = 0.0;
// The game's system (services + 0x170, the ISystem that ISystem::Quit is called on) and its
// developer mode flags (CSystem::SetDevMode).
constexpr std::size_t kServicesSystem = 0x170;
constexpr std::size_t kSystemWasDevMode = 1489;
constexpr std::size_t kSystemDevMode = 1490;

// Set once the player chose a tool in the main menu (start_from_menu), cleared by its Leave.
std::atomic<bool> started_from_menu{false};
// The developer mode flags as the tool found them, given back on Leave.
std::uint8_t was_dev_mode_before = 0;
std::uint8_t dev_mode_before = 0;
// The editor's screen drew its first frame ({ op: 'editorShown' }) since the tool started.
std::atomic<bool> shown{false};
// The editor's Leave, served on the engine thread (take_leave) a frame after it came: the editor
// sends the removal of its props first, and the studio's own ticks serve those in that frame.
std::atomic<int> leave_frames{0};

// Whether the game's command line holds its developer mode switch.
bool developer_mode() {
  const wchar_t* line = GetCommandLineW();
  if (!line) {
    return false;
  }
  std::wstring lower(line);
  for (wchar_t& c : lower) {
    c = static_cast<wchar_t>(towlower(c));
  }
  return lower.find(L" -devmode") != std::wstring::npos;
}

bool asked() {
  const char* value = std::getenv("KCDS_WORLD_ARCHITECT");
  return value && std::string(value) == "1";
}

bool decide() {
  const bool on = developer_mode() && asked();
  if (asked() && !on) {
    kcds::game_client::log("WORLD ARCHITECT asked for, but the game is not in developer mode");
  }
  if (on) {
    kcds::game_client::log("WORLD ARCHITECT local mode: no server, map %s", world().map.c_str());
  }
  return on;
}

// The assistant's script-step pipe opens with the first frame of the local mode
// (editor_pipe::start asks enabled(), so not from decide()).
void open_bridge() {
  static bool opened = false;
  if (!opened) {
    opened = true;
    editor_pipe::start();
  }
}

// Set on the engine thread, read on the network thread too (the key gate).
std::atomic<bool> loaded{false};

// The map the author switched to in the editor (empty: the start's), and the world's revision:
// a new one makes the client load the debug world again (core/frame.cpp request_server_world).
std::mutex map_mutex;
std::string switched_map;
std::atomic<std::uint32_t> revision{1};
// Set by a switch: the editor opens again once the new world is ready.
std::atomic<bool> reopen{false};

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

// Test builds only (KCDS_TEST_HOOKS): the kit drives the editor in the one local window through
// Local\KCD2S.ArchitectScript.<pid>; each line of %TEMP%\kcds-architect-<pid>.txt goes to the
// editor as { op: 'architectTest', line } (Client/resources/architect/editor.cjs script()).
HANDLE test_signal = nullptr;
bool test_signal_made = false;

std::string test_script_path() {
  wchar_t temp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  char path[MAX_PATH * 2] = {};
  std::snprintf(path, sizeof(path), "%lskcds-architect-%lu.txt", temp, GetCurrentProcessId());
  return path;
}

// A virtual function of an object by its vtable index.
template <class Fn>
Fn virtual_call(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

// "cvar <name> <whole number>": the engine console's variable set as the console would (GetCVar,
// console slot 23; ICVar Set(int), slot 7), for live experiments.
void set_test_cvar(const std::string& text) {
  char name[128] = {};
  int value = 0;
  if (std::sscanf(text.c_str(), "%127s %d", name, &value) != 2) {
    return;
  }
  void* console = kcds::game_client::console_slot ? *kcds::game_client::console_slot : nullptr;
  if (!console) {
    return;
  }
  using GetCVar = void*(__fastcall*)(void* console, const char* name);
  using SetInt = void(__fastcall*)(void* variable, int value);
  void* variable = virtual_call<GetCVar>(console, 23)(console, name);
  if (!variable) {
    kcds::game_client::log("ARCHITECT TEST cvar %s unknown", name);
    return;
  }
  virtual_call<SetInt>(variable, 7)(variable, value);
  kcds::game_client::log("ARCHITECT TEST cvar %s = %d", name, value);
}

// "grass hide|show <id> <x> <y> <radius>": the nature brush's grass order without the editor,
// so a test changes no project.
void test_grass(const std::string& text) {
  char kind[8] = {};
  unsigned id = 0;
  double x = 0.0;
  double y = 0.0;
  double radius = 0.0;
  if (std::sscanf(text.c_str(), "%7s %u %lf %lf %lf", kind, &id, &x, &y, &radius) != 5) {
    return;
  }
  terrain::ops::handle_runtime_message(sdk::Value(sdk::Value::Map{
      {"op", sdk::Value("architectGrass")},
      {"kind", sdk::Value(std::string(kind))},
      {"id", sdk::Value(std::int64_t(id))},
      {"x", sdk::Value(x)},
      {"y", sdk::Value(y)},
      {"radius", sdk::Value(radius)},
  }));
  kcds::game_client::log("ARCHITECT TEST grass %s %u at %.1f %.1f r %.1f", kind, id, x, y, radius);
}

void run_test_script(const std::function<bool(const sdk::Value&)>& send) {
  if (!test_signal_made) {
    test_signal_made = true;
    if (GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) != 0) {
      wchar_t name[96];
      swprintf_s(name, L"Local\\KCD2S.ArchitectScript.%lu", GetCurrentProcessId());
      test_signal = CreateEventW(nullptr, FALSE, FALSE, name);
    }
  }
  if (!test_signal || WaitForSingleObject(test_signal, 0) != WAIT_OBJECT_0) {
    return;
  }
  std::ifstream file(test_script_path());
  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("cvar ", 0) == 0) {
      set_test_cvar(line.substr(5));
      continue;
    }
    // The Asset/Animation Studio's own test lines.
    if (line.rfind("studio ", 0) == 0) {
      studio::rope::command(line.substr(7));
      continue;
    }
    if (line.rfind("grass ", 0) == 0) {
      test_grass(line.substr(6));
      continue;
    }
    if (!line.empty() && line.size() < 512) {
      const char* operation = studio::requested() ? "studioTest" : "architectTest";
      send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(operation)}, {"line", sdk::Value(line)}}));
    }
  }
}

// Opens the editor and tells it which pictures the disk keeps. The Asset/Animation Studio has
// its own editor (Client/resources/studio/editor.cjs).
bool sendOpen(const std::function<bool(const sdk::Value&)>& send) {
  if (studio::requested()) {
    // The studio's project kept on this machine, rebuilt by its editor as it opens.
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioProject")},
                                    {"data", sdk::Value(projects::load(studio::kLevel))}}));
    return send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioOpen")}}));
  }
  sdk::Value::Array keys;
  for (const std::string& key : overlay::cached_keys()) {
    keys.push_back(sdk::Value(key));
  }
  send(sdk::Value(
      sdk::Value::Map{{"op", sdk::Value("architectPicturesKept")}, {"keys", sdk::Value(keys)}}));
  // The project kept for this map, rebuilt by the editor as it opens. A message holds 64 KiB:
  // a larger project goes in parts (cut between whole UTF-8 characters), joined by the editor.
  const std::string map = world().map;
  const std::string data = projects::load(map);
  constexpr std::size_t kPart = 40000;
  std::vector<std::string> parts;
  for (std::size_t start = 0; start < data.size() || parts.empty();) {
    std::size_t end = (std::min)(data.size(), start + kPart);
    while (end < data.size() && end > start && (std::uint8_t(data[end]) & 0xC0) == 0x80) {
      --end;
    }
    parts.push_back(data.substr(start, end - start));
    start = end;
    if (data.empty()) {
      break;
    }
  }
  for (std::size_t index = 0; index < parts.size(); ++index) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("architectProject")},
                                    {"map", sdk::Value(map)},
                                    {"data", sdk::Value(parts[index])},
                                    {"part", sdk::Value(std::int64_t(index))},
                                    {"parts", sdk::Value(std::int64_t(parts.size()))}}));
  }
  return send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("architectOpen")}}));
}

}  // namespace

bool enabled() {
  static const bool at_start = decide();
  return at_start || started_from_menu.load();
}

bool start_from_menu(bool studio) {
  if (enabled()) {
    return true;
  }
  auto* services = kcds::game_client::get_services
                       ? static_cast<std::uint8_t*>(kcds::game_client::get_services())
                       : nullptr;
  auto* system = services ? *reinterpret_cast<std::uint8_t**>(services + kServicesSystem) : nullptr;
  if (!system) {
    kcds::game_client::log("WORLD ARCHITECT not started from the menu: no game system yet");
    return false;
  }
  // As CSystem::SetDevMode(true) at the game's start; what was there goes back on Leave.
  was_dev_mode_before = system[kSystemWasDevMode];
  dev_mode_before = system[kSystemDevMode];
  system[kSystemWasDevMode] = 1;
  system[kSystemDevMode] = 1;
  // The tool's variables, as a start in the tool has them (read with getenv and the Windows API).
  _putenv_s("KCDS_WORLD_ARCHITECT", "1");
  _putenv_s("KCDS_ASSET_STUDIO", studio ? "1" : "0");
  SetEnvironmentVariableW(L"KCDS_WORLD_ARCHITECT", L"1");
  SetEnvironmentVariableW(L"KCDS_ASSET_STUDIO", studio ? L"1" : L"0");
  if (studio) {
    studio::provide_level();
  }
  shown.store(false);
  started_from_menu.store(true);
  kcds::game_client::log("WORLD ARCHITECT local mode from the main menu: %s, map %s",
                         studio ? "the Asset/Animation Studio" : "World Architect",
                         world().map.c_str());
  return true;
}

integration::WorldConfig world() {
  integration::WorldConfig config;
  config.revision = revision.load();
  config.debug = true;
  config.quests = false;
  config.reveal_map = true;
  config.level_people = false;
  config.map = "trosecko";
  if (const char* map = std::getenv("KCDS_WORLD_ARCHITECT_MAP")) {
    integration::WorldConfig wanted = config;
    wanted.map = map;
    // Only the maps a server may name; anything else keeps trosecko.
    if (integration::valid_world_config(wanted)) {
      config.map = wanted.map;
    }
  }
  // The Asset/Animation Studio loads no game map at all: its own empty level,
  // which only this local mode can name (never a server).
  if (studio::requested()) {
    config.map = studio::kLevel;
    return config;
  }
  std::lock_guard<std::mutex> lock(map_mutex);
  if (!switched_map.empty()) {
    config.map = switched_map;
  }
  return config;
}

bool take_leave() {
  // Counted down here only (the engine thread); the editor's message sets it.
  const int frames = leave_frames.load();
  if (frames <= 0) {
    return false;
  }
  leave_frames.store(frames - 1);
  return frames == 1;
}

bool stop_from_menu() {
  if (!started_from_menu.load()) {
    return false;
  }
  auto* services = kcds::game_client::get_services
                       ? static_cast<std::uint8_t*>(kcds::game_client::get_services())
                       : nullptr;
  auto* system = services ? *reinterpret_cast<std::uint8_t**>(services + kServicesSystem) : nullptr;
  if (system) {
    system[kSystemWasDevMode] = was_dev_mode_before;
    system[kSystemDevMode] = dev_mode_before;
  }
  _putenv_s("KCDS_WORLD_ARCHITECT", "");
  _putenv_s("KCDS_ASSET_STUDIO", "");
  SetEnvironmentVariableW(L"KCDS_WORLD_ARCHITECT", nullptr);
  SetEnvironmentVariableW(L"KCDS_ASSET_STUDIO", nullptr);
  {
    std::lock_guard<std::mutex> lock(map_mutex);
    switched_map.clear();
  }
  loaded.store(false);
  // The next tool started from the menu opens its editor again (frame), and draws it anew.
  reopen.store(true);
  shown.store(false);
  revision.fetch_add(1);
  started_from_menu.store(false);
  kcds::game_client::log("WORLD ARCHITECT left: back to the main menu");
  return true;
}

bool editor_shown() {
  return shown.load();
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "editorShown") {
    if (enabled() && !shown.exchange(true)) {
      kcds::game_client::log("WORLD ARCHITECT editor drawn");
    }
    return true;
  }
  if (operation == "architectLeave") {
    if (enabled()) {
      leave_frames.store(2);
    }
    return true;
  }
  if (operation != "architectMap") {
    return false;
  }
  if (!enabled()) {
    return true;
  }
  integration::WorldConfig wanted = world();
  wanted.map = text(message, "map");
  if (wanted.map == world().map || !integration::valid_world_config(wanted)) {
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(map_mutex);
    switched_map = wanted.map;
  }
  loaded.store(false);
  reopen.store(true);
  revision.fetch_add(1);
  kcds::game_client::log("WORLD ARCHITECT switching to map %s", wanted.map.c_str());
  return true;
}

void mark_world_ready() {
  loaded.store(true);
}

bool world_ready() {
  return loaded.load();
}

void frame(bool in_world, unsigned runtime, const std::function<bool(const sdk::Value&)>& send) {
  static double ready_since = -1.0;
  static bool opened = false;
  static bool f10_down = false;
  static unsigned opened_in = 0;
  if (reopen.exchange(false)) {
    // A map switch: the editor closed itself and opens again in the new world.
    opened = false;
    ready_since = -1.0;
  }
  if (opened && runtime != opened_in) {
    // The runtime was started anew (it ends when its browser process fails): open it again.
    opened = false;
    ready_since = -1.0;
  }
  open_bridge();
  // The world as this frame found it, and still loaded now: a map switch clears `loaded` before it
  // asks for the reopen, so the editor does not open again in the world being left (live
  // it opened at once, then not again in the new map, its camera off).
  if (!in_world || !loaded.load()) {
    ready_since = -1.0;
    return;
  }
  const double now = GetTickCount64() / 1000.0;
  if (ready_since < 0.0) {
    ready_since = now;
  }
  if (opened) {
    run_test_script(send);
    // The assistant's steps (editor_pipe.hpp), to the open editor with their id for the answer.
    for (const editor_pipe::Step& step : editor_pipe::take()) {
      const char* operation = studio::requested() ? "studioTest" : "architectTest";
      send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(operation)},
                                      {"line", sdk::Value(step.line)},
                                      {"id", sdk::Value(step.id)}}));
    }
  }
  if (!opened && now - ready_since >= kOpenDelaySeconds) {
    opened = sendOpen(send);
    opened_in = runtime;
    if (opened) {
      kcds::game_client::log("WORLD ARCHITECT opened");
    }
  }
  DWORD process = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process);
  const bool front = process == GetCurrentProcessId();
  const bool down = front && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  if (down && !f10_down && opened) {
    sendOpen(send);
  }
  f10_down = down;
  // The mouse wheel for the editor's panels (a long Details list scrolls): notches, away from
  // you positive.
  const int turned = kcd2s::native_chat::take_wheel();
  if (opened && turned != 0) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("editorWheel")},
                                    {"notches", sdk::Value(double(turned) / WHEEL_DELTA)}}));
  }
  // Playing as the player (the editor closed): Esc goes back to the editor, as F10 does; the
  // editor keeps the game's pause menu (its open_pause_menu action) blocked meanwhile.
  static bool escape_down = false;
  const bool escape = front && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
  if (escape && !escape_down && opened && !player_body::editor_open()) {
    sendOpen(send);
  }
  escape_down = escape;
}

}  // namespace kcds::runtime::architect::local_mode
