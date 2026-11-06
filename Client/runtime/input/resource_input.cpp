#include "resource_input.hpp"

// Winsock before Windows.h (log.hpp).
#include "../core/log.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::input {
namespace {

namespace sdk = kcd2s::sdk;

enum class Kind { Virtual, Scan, Pad };

struct Key {
  std::string name;
  Kind kind = Kind::Virtual;  // code: a virtual key, a set 1 scan code, or a controller button
  unsigned code = 0;
};

// Controller buttons: XINPUT_GAMEPAD_* bits, and the two triggers as buttons.
constexpr unsigned kLeftTrigger = 0x10000;
constexpr unsigned kRightTrigger = 0x20000;
constexpr unsigned kPadCodes = 0x40000;
constexpr BYTE kTriggerDown = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

using GetPadState = DWORD(WINAPI*)(DWORD user, XINPUT_STATE* state);

std::mutex table_mutex;
std::vector<Key> keys;
std::atomic<bool> disabled{false};
std::atomic<bool> cursor{false};
// The game actions the resources hold off (Controls.blockAction).
std::mutex actions_mutex;
std::set<std::string> disabled_actions;
// The actions whose last press was held off (hold_off): their release is held off too.
std::set<std::string> held_off_presses;

// The current session's view of what it was told.
const void* reported_session = nullptr;
std::set<std::string> previous_down;
bool gate_was_open = false;
HKL labels_layout = nullptr;
std::size_t labels_count = 0;
bool reported_captured = false;
bool reported_pointer = false;

// Where the mouse pointer is, as the scripts last heard it (Controls.cursorPosition): in the game
// window's client pixels, or off (no pointer of the scripts', or outside the window).
struct Cursor {
  bool on = false;
  long x = 0;
  long y = 0;
};
Cursor reported_cursor;

// While a resource shows the pointer (Controls.showCursor), where it is: { op: 'cursorAt', x, y },
// -1 .. 1 left to right and bottom to top as Camera.rayFromScreen takes them, or x and y null
// outside the window; sent only when it moved.
template <class Send, class Gate>
void report_cursor(const Send& send, const Gate& gate) {
  HWND window = static_cast<HWND>(gate.window);
  POINT point{};
  RECT client{};
  Cursor now;
  if (cursor.load() && gate.in_world && window && GetCursorPos(&point) &&
      ScreenToClient(window, &point) && GetClientRect(window, &client) && client.right > 0 &&
      client.bottom > 0 && point.x >= 0 && point.y >= 0 && point.x < client.right &&
      point.y < client.bottom) {
    now.on = true;
    now.x = point.x;
    now.y = point.y;
  }
  if (now.on == reported_cursor.on && now.x == reported_cursor.x && now.y == reported_cursor.y) {
    return;
  }
  sdk::Value::Map message{{"op", sdk::Value(std::string("cursorAt"))}};
  if (now.on) {
    message.emplace_back("x", sdk::Value(double(now.x) / double(client.right) * 2.0 - 1.0));
    message.emplace_back("y", sdk::Value(1.0 - double(now.y) / double(client.bottom) * 2.0));
  } else {
    message.emplace_back("x", sdk::Value());
    message.emplace_back("y", sdk::Value());
  }
  if (send(sdk::Value(std::move(message)))) {
    reported_cursor = now;
  }
}

std::string text_field(const sdk::Value& value, const char* name) {
  const sdk::Value* field = kcd2s::integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

std::int64_t number_field(const sdk::Value& value, const char* name) {
  const sdk::Value* field = kcd2s::integration::script_field(value, name);
  if (!field) {
    return 0;
  }
  if (field->tag() == sdk::Tag::Int) {
    return field->as_int();
  }
  if (field->tag() == sdk::Tag::Double) {
    return static_cast<std::int64_t>(field->as_double());
  }
  return 0;
}

bool flag_field(const sdk::Value& value, const char* name) {
  const sdk::Value* field = kcd2s::integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Bool && field->as_bool();
}

void read_table(const sdk::Value& message) {
  const sdk::Value* list = kcd2s::integration::script_field(message, "keys");
  if (!list || list->tag() != sdk::Tag::Array) {
    return;
  }
  std::vector<Key> table;
  for (const sdk::Value& entry : list->as_array()) {
    Key key;
    key.name = text_field(entry, "name");
    const std::string kind = text_field(entry, "kind");
    key.kind = kind == "scan" ? Kind::Scan : kind == "pad" ? Kind::Pad : Kind::Virtual;
    key.code = static_cast<unsigned>(number_field(entry, "code"));
    const unsigned limit = key.kind == Kind::Pad ? kPadCodes : 0x100;
    if (!key.name.empty() && key.code > 0 && key.code < limit) {
      table.push_back(key);
    }
  }
  std::lock_guard<std::mutex> lock(table_mutex);
  keys = std::move(table);
}

unsigned virtual_key(const Key& key, HKL layout) {
  if (key.kind != Kind::Scan) {
    return key.code;
  }
  return MapVirtualKeyExW(key.code, MAPVK_VSC_TO_VK_EX, layout);
}

std::string utf8(const wchar_t* text, int length) {
  int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
  return out;
}

// What the player's layout prints on the scan-code keys, upper case.
sdk::Value labels_for(const std::vector<Key>& table, HKL layout) {
  sdk::Value::Map labels;
  BYTE state[256]{};
  for (const Key& key : table) {
    if (key.kind != Kind::Scan) {
      continue;
    }
    wchar_t characters[4]{};
    unsigned vk = virtual_key(key, layout);
    int count = ToUnicodeEx(vk, key.code, state, characters, 4, 4, layout);
    if (count < 0) {
      count = 1;  // a dead key: its accent is the label
    }
    if (count <= 0 || characters[0] < 0x20) {
      continue;
    }
    CharUpperBuffW(characters, static_cast<DWORD>(count));
    labels.emplace_back(key.name, sdk::Value(utf8(characters, count)));
  }
  return sdk::Value(std::move(labels));
}

// The buttons held on any connected controller (XInput, loaded when first asked), with the
// triggers as kLeftTrigger and kRightTrigger.
unsigned pad_buttons() {
  static GetPadState get_state = [] {
    HMODULE xinput = LoadLibraryW(L"xinput1_4.dll");
    if (!xinput) {
      xinput = LoadLibraryW(L"xinput9_1_0.dll");
    }
    return xinput ? reinterpret_cast<GetPadState>(GetProcAddress(xinput, "XInputGetState"))
                  : nullptr;
  }();
  if (!get_state) {
    return 0;
  }
  unsigned buttons = 0;
  for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
    XINPUT_STATE state{};
    if (get_state(user, &state) != ERROR_SUCCESS) {
      continue;
    }
    buttons |= state.Gamepad.wButtons;
    if (state.Gamepad.bLeftTrigger > kTriggerDown) {
      buttons |= kLeftTrigger;
    }
    if (state.Gamepad.bRightTrigger > kTriggerDown) {
      buttons |= kRightTrigger;
    }
  }
  return buttons;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  std::string op = kcd2s::integration::script_operation(message);
  if (op == "keyTable") {
    read_table(message);
    return true;
  }
  if (op == "controls") {
    disabled.store(flag_field(message, "disabled"));
    cursor.store(flag_field(message, "cursor"));
    std::set<std::string> actions;
    const sdk::Value* list = kcd2s::integration::script_field(message, "actions");
    if (list && list->tag() == sdk::Tag::Array) {
      for (const sdk::Value& entry : list->as_array()) {
        if (entry.tag() == sdk::Tag::Str && !entry.as_str().empty() &&
            entry.as_str().size() <= 64) {
          actions.insert(entry.as_str());
        }
      }
    }
    std::lock_guard<std::mutex> lock(actions_mutex);
    disabled_actions = std::move(actions);
    return true;
  }
  return false;
}

bool controls_disabled() {
  return disabled.load();
}

bool controls_cursor() {
  return cursor.load();
}

bool action_disabled(const char* action) {
  if (!action) {
    return false;
  }
  std::lock_guard<std::mutex> lock(actions_mutex);
  return !disabled_actions.empty() && disabled_actions.count(action) > 0;
}

bool sitting_map_held() {
  std::lock_guard<std::mutex> lock(actions_mutex);
  for (const char* exit :
       {"sitting_exit_up", "sitting_exit_down", "sitting_exit_left", "sitting_exit_right"}) {
    if (disabled_actions.count(exit) > 0) {
      return true;
    }
  }
  return false;
}

bool hold_off(const char* action, unsigned mode) {
  if (!action) {
    return false;
  }
  constexpr unsigned kPress = 1;
  constexpr unsigned kRelease = 2;
  std::lock_guard<std::mutex> lock(actions_mutex);
  const bool blocked = !disabled_actions.empty() && disabled_actions.count(action) > 0;
  if (mode == kRelease) {
    return held_off_presses.count(action) > 0;
  }
  if (mode == kPress) {
    if (blocked) {
      held_off_presses.insert(action);
    } else {
      held_off_presses.erase(action);
    }
  }
  return blocked;
}

void pump(const void* session, const RuntimeSender& send, const Gate& gate) {
  if (session != reported_session) {
    reported_session = session;  // a new runtime: it has heard nothing, and it starts unbound
    reported_cursor = Cursor{};
    previous_down.clear();
    gate_was_open = false;
    labels_layout = nullptr;
    labels_count = 0;
    reported_captured = false;
    reported_pointer = false;
    disabled.store(false);
    cursor.store(false);
    {
      std::lock_guard<std::mutex> actions_lock(actions_mutex);
      disabled_actions.clear();
    }
    std::lock_guard<std::mutex> lock(table_mutex);
    keys.clear();
  }
  if (gate.captured != reported_captured || gate.pointer != reported_pointer) {
    sdk::Value::Map host{{"op", sdk::Value(std::string("inputHost"))},
                         {"captured", sdk::Value(gate.captured)},
                         {"pointer", sdk::Value(gate.pointer)}};
    if (send(sdk::Value(std::move(host)))) {
      reported_captured = gate.captured;
      reported_pointer = gate.pointer;
    }
  }
  report_cursor(send, gate);
  std::vector<Key> table;
  {
    std::lock_guard<std::mutex> lock(table_mutex);
    table = keys;
  }
  if (table.empty()) {
    return;
  }
  HWND window = static_cast<HWND>(gate.window);
  HKL layout = GetKeyboardLayout(window ? GetWindowThreadProcessId(window, nullptr) : 0);
  if (layout != labels_layout || table.size() != labels_count) {
    sdk::Value::Map message{{"op", sdk::Value(std::string("keyLabels"))},
                            {"labels", labels_for(table, layout)}};
    if (send(sdk::Value(std::move(message)))) {
      labels_layout = layout;
      labels_count = table.size();
    }
  }

  // Binds fire only in a world, with the game window in front and nothing capturing typed input.
  const HWND front = GetForegroundWindow();
  const bool open = gate.in_world && !gate.captured && window && front == window;
  // The one part of the gate the client's KEYS gate line does not show: logged once each time
  // the game window stops or starts being in front while the rest of the gate is open
  // (World Architect's right mouse button did nothing after the studio's Leave).
  static int front_reported = -1;
  const int in_front = front == window ? 1 : 0;
  if (gate.in_world && !gate.captured && window && in_front != front_reported) {
    front_reported = in_front;
    DWORD front_process = 0;
    GetWindowThreadProcessId(front, &front_process);
    kcds::game_client::log("KEYS game window %s (in front: window %p of process %lu, own %lu)",
                           in_front ? "in front" : "not in front", static_cast<void*>(front),
                           front_process, GetCurrentProcessId());
  }
  std::set<std::string> down;
  bool pad_read = false;
  unsigned pad = 0;
  for (const Key& key : table) {
    if (key.kind == Kind::Pad) {
      if (!pad_read) {
        pad = pad_buttons();
        pad_read = true;
      }
      if (pad & key.code) {
        down.insert(key.name);
      }
    } else if (GetAsyncKeyState(static_cast<int>(virtual_key(key, layout))) & 0x8000) {
      down.insert(key.name);
    }
  }
  if (!open) {
    if (gate_was_open && !previous_down.empty()) {
      sdk::Value::Map cleared{{"op", sdk::Value(std::string("keyEdges"))},
                              {"edges", sdk::Value(sdk::Value::Array{})},
                              {"down", sdk::Value(sdk::Value::Array{})}};
      send(sdk::Value(std::move(cleared)));
    }
    gate_was_open = false;
    previous_down = std::move(down);  // keys held now do not fire when the gate opens
    return;
  }
  if (!gate_was_open) {
    // Keys already held when the gate opens fire nothing; they only count for isDown.
    gate_was_open = true;
    previous_down = down;
    sdk::Value::Array held;
    for (const std::string& name : down) {
      held.emplace_back(name);
    }
    sdk::Value::Map state{{"op", sdk::Value(std::string("keyEdges"))},
                          {"edges", sdk::Value(sdk::Value::Array{})},
                          {"down", sdk::Value(std::move(held))}};
    send(sdk::Value(std::move(state)));
    return;
  }
  if (down == previous_down) {
    return;
  }
  sdk::Value::Array edges;
  for (const std::string& name : down) {
    if (previous_down.count(name) == 0) {
      edges.emplace_back(sdk::Value::Array{sdk::Value(name), sdk::Value(std::string("down"))});
    }
  }
  for (const std::string& name : previous_down) {
    if (down.count(name) == 0) {
      edges.emplace_back(sdk::Value::Array{sdk::Value(name), sdk::Value(std::string("up"))});
    }
  }
  sdk::Value::Array current;
  for (const std::string& name : down) {
    current.emplace_back(name);
  }
  sdk::Value::Map message{{"op", sdk::Value(std::string("keyEdges"))},
                          {"edges", sdk::Value(std::move(edges))},
                          {"down", sdk::Value(std::move(current))}};
  send(sdk::Value(std::move(message)));
  previous_down = std::move(down);
}

}  // namespace kcds::runtime::input
