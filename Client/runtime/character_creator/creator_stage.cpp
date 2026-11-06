#include "creator_stage.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../action_hints/resource_action_hints.hpp"
#include "../appearance/components.hpp"
#include "../camera/anchor_view.hpp"
#include "../native_ui/native_ui.hpp"
#include "kcd2s/integration/appearance.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::character_creator {
namespace {

namespace sdk = kcd2s::sdk;
using kcd2s::integration::Appearance;

// The pause screen: services + 432, its vtable (byte offsets) and its open state machine.
constexpr std::size_t kServicesApse = 432;
constexpr std::size_t kApseOpen = 8;
constexpr std::size_t kApseClose = 16;
constexpr std::size_t kApsePage = 24;
constexpr std::size_t kApseState = 104;
constexpr std::size_t kStateGet = 8;
constexpr std::uint8_t kPlayerPage = 4;

// The character stage and the player page inside the pause screen, and what they hold.
constexpr std::size_t kApseStage = 8520;
constexpr std::size_t kStageInitSlot = 14;
constexpr std::size_t kStageDummy = 32;
constexpr std::size_t kStagePanel = 424;
constexpr std::size_t kApsePlayer = 2960;
constexpr std::size_t kPlayerListPlate = 216;
constexpr std::size_t kPlayerInfoPlate = 688;

// CEntity: vtable slot 65 sets its invisible flag, slot 66 reads it.
constexpr std::size_t kSetInvisibleSlot = 65;
constexpr std::size_t kIsInvisibleSlot = 66;

// Open's context: the global's value, then an empty std::function (its target at + 56 null).
struct OpenContext {
  std::uint64_t value = 0;
  std::uint8_t function[64] = {};
};

using ApseOpen = bool(__fastcall*)(void* apse, std::uint8_t page, void* context);
using ApseClose = void(__fastcall*)(void* apse, bool instant);
using ApseGet = std::uint8_t(__fastcall*)(void* object);
using SetInvisible = void(__fastcall*)(void* entity, bool invisible, bool recursive);
using IsInvisible = bool(__fastcall*)(void* entity);

enum class Phase { kIdle, kOpening, kOpen };

// CEntity: the actor's entity (actor + 56), its world matrix (vtable byte offset 256).
constexpr std::size_t kActorEntity = 56;
constexpr std::size_t kGetWorldTM = 256;
// The creator's framings: how far in front of the figure, how high above its feet the camera
// looks, how far to the camera's right (so the figure sits in the left half of the screen), and
// the vertical field of view in degrees.
struct Framing {
  double distance;
  double height;
  double side;
  double fov;
};
constexpr Framing kWholeFigure{4.6, 0.95, 1.0, 30};
constexpr Framing kHead{1.25, 1.62, 0.27, 30};
// The share of the way to the wanted framing taken each frame.
constexpr double kEase = 0.15;
constexpr double kPi = 3.14159265358979323846;

// The creator's keys, by their place on the keyboard (scan codes), so every layout has them where
// a US keyboard has them; the panel names them as the layout does.
struct CreatorKey {
  const char* action;
  unsigned scan;
};
constexpr CreatorKey kKeys[] = {
    {"gender", 0x22},  // G
    {"random", 0x13},  // R
    {"reset", 0x0E},   // Backspace
    {"accept", 0x1C},  // Enter
    {"up", 0x148},     // Up
    {"down", 0x150},   // Down
    {"left", 0x14B},   // Left
    {"right", 0x14D},  // Right
    {"tab", 0x0F},     // Tab
};
// The pause screen closes on its own key: the panel only names it.
constexpr unsigned kCancelScan = 0x01;  // Esc

Logger log_line = nullptr;
Natives natives;

// Requests from the runtime, taken by the engine thread.
std::mutex request_mutex;
bool open_requested = false;
bool close_requested = false;
std::optional<Appearance> requested_look;
std::optional<bool> requested_frame;

// Engine thread only.
Phase phase = Phase::kIdle;
Appearance look;
bool look_changed = false;
std::vector<void*> hidden;
camera::AnchorView view("kcds_creator_camera");
bool close_framing = false;
camera::Pose shown_pose;
bool shown_pose_valid = false;

// Stage changes and key presses for the runtime, sent by the network thread.
std::mutex state_mutex;
std::vector<sdk::Value> changes;
bool keys_down[sizeof(kKeys) / sizeof(kKeys[0])] = {};

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[offset / sizeof(void*)]);
}

template <class T>
T read(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

void queue(sdk::Value::Map message) {
  std::lock_guard<std::mutex> lock(state_mutex);
  changes.emplace_back(std::move(message));
}

std::string key_name(unsigned scan) {
  wchar_t wide[64] = {};
  const LONG param = static_cast<LONG>(((scan & 0xFF) << 16) | ((scan & 0x100) ? (1 << 24) : 0));
  const int length = GetKeyNameTextW(param, wide, 64);
  char utf8[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, wide, length, utf8, sizeof(utf8) - 1, nullptr, nullptr);
  return utf8;
}

void report(const char* state) {
  if (log_line) {
    log_line("CREATOR stage %s", state);
  }
  sdk::Value::Map message{{"op", sdk::Value(std::string("creatorStage"))},
                          {"state", sdk::Value(std::string(state))}};
  if (std::string(state) == "open") {
    sdk::Value::Map labels;
    for (const CreatorKey& key : kKeys) {
      labels.emplace_back(key.action, sdk::Value(key_name(key.scan)));
    }
    labels.emplace_back("cancel", sdk::Value(key_name(kCancelScan)));
    message.emplace_back("keys", sdk::Value(std::move(labels)));
  }
  queue(std::move(message));
}

// Presses of the creator's keys while the game window is in front; the pause screen holds the
// game's input, so the resources' own key binds do not fire.
void read_keys() {
  void* window = natives.window ? natives.window() : nullptr;
  const bool front = window && GetForegroundWindow() == static_cast<HWND>(window);
  HKL layout = GetKeyboardLayout(0);
  for (std::size_t index = 0; index < sizeof(kKeys) / sizeof(kKeys[0]); ++index) {
    // An extended key (the arrows) is asked for with its E0 prefix.
    const unsigned scan = kKeys[index].scan;
    const UINT code = (scan & 0x100) != 0 ? (0xE000 | (scan & 0xFF)) : scan;
    const UINT virtual_key = MapVirtualKeyExW(code, MAPVK_VSC_TO_VK_EX, layout);
    const bool down = front && virtual_key && (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
    if (down && !keys_down[index]) {
      queue({{"op", sdk::Value(std::string("creatorKey"))},
             {"key", sdk::Value(std::string(kKeys[index].action))}});
    }
    keys_down[index] = down;
  }
}

void* apse() {
  void* services = natives.services ? natives.services() : nullptr;
  return services ? read<void*>(services, kServicesApse) : nullptr;
}

// The pause screen's character stage and player page, checked against this build's vtables.
void* stage_of(void* menu) {
  auto* stage = static_cast<std::uint8_t*>(menu) + kApseStage;
  void* const* vtable = read<void* const*>(stage, 0);
  return vtable && vtable[kStageInitSlot] == natives.stage_init ? stage : nullptr;
}

void* player_page_of(void* menu) {
  auto* page = static_cast<std::uint8_t*>(menu) + kApsePlayer;
  return read<const void*>(page, 0) == natives.apse_player_vtable ? page : nullptr;
}

bool menu_closed(void* menu) {
  void* state = static_cast<std::uint8_t*>(menu) + kApseState;
  const std::uint8_t step = slot<ApseGet>(state, kStateGet)(state);
  return step == 0 || step == 1;
}

std::uint8_t menu_page(void* menu) {
  return slot<ApseGet>(menu, kApsePage)(menu);
}

// The entities a creator does not draw: the two plates and the character panel.
std::vector<void*> panels_of(void* menu) {
  std::vector<void*> panels;
  void* stage = stage_of(menu);
  void* page = player_page_of(menu);
  if (!stage || !page) {
    return panels;
  }
  for (void* entity : {read<void*>(page, kPlayerListPlate), read<void*>(page, kPlayerInfoPlate),
                       read<void*>(stage, kStagePanel)}) {
    if (entity) {
      panels.push_back(entity);
    }
  }
  return panels;
}

// Keeps the panels undrawn; remembers only those the game drew, to draw them again.
void hide_panels(void* menu) {
  for (void* entity : panels_of(menu)) {
    if (slot<IsInvisible>(entity, kIsInvisibleSlot * sizeof(void*))(entity)) {
      continue;
    }
    slot<SetInvisible>(entity, kSetInvisibleSlot * sizeof(void*))(entity, true, false);
    hidden.push_back(entity);
  }
}

void show_panels() {
  for (void* entity : hidden) {
    slot<SetInvisible>(entity, kSetInvisibleSlot * sizeof(void*))(entity, false, false);
  }
  hidden.clear();
}

void dress_dummy(void* menu) {
  void* stage = stage_of(menu);
  void* dummy = stage ? read<void*>(stage, kStageDummy) : nullptr;
  if (dummy && !appearance::components::dress_dummy(dummy, look) && log_line) {
    log_line("CREATOR look gender=%s refused", look.gender.c_str());
  }
}

// The camera pose of a framing: in front of the figure (along its forward, the matrix's second
// column), looking back at it.
camera::Pose framing_pose(const float* figure, const Framing& framing) {
  const double feet[3] = {figure[3], figure[7], figure[11]};
  double forward[3] = {figure[1], figure[5], 0};
  const double length = std::hypot(forward[0], forward[1]);
  forward[0] = length > 0 ? forward[0] / length : 0;
  forward[1] = length > 0 ? forward[1] / length : -1;
  // The camera looks along -forward; its right is then (-forward.y, forward.x).
  const double right[2] = {-forward[1], forward[0]};
  camera::Pose pose;
  pose.x = feet[0] + forward[0] * framing.distance + right[0] * framing.side;
  pose.y = feet[1] + forward[1] * framing.distance + right[1] * framing.side;
  pose.z = feet[2] + framing.height;
  pose.yaw = std::atan2(forward[0], -forward[1]) * 180 / kPi;
  pose.pitch = 0;
  return pose;
}

double ease_angle(double from, double to) {
  double delta = std::fmod(to - from + 540.0, 360.0) - 180.0;
  return from + delta * kEase;
}

// Keeps the creator's camera on the figure; it begins with the stage.
void film(void* menu) {
  void* stage = stage_of(menu);
  void* dummy = stage ? read<void*>(stage, kStageDummy) : nullptr;
  void* entity = dummy ? read<void*>(dummy, kActorEntity) : nullptr;
  if (!entity) {
    return;
  }
  if (!view.active()) {
    camera::Natives camera_natives;
    camera_natives.framework = natives.framework;
    camera_natives.entity_system = natives.entity_system;
    camera_natives.spawn_params_init = natives.spawn_params_init;
    if (!view.begin(camera_natives, nullptr, nullptr)) {
      return;
    }
    shown_pose_valid = false;
  }
  const float* figure = slot<const float*(__fastcall*)(void*)>(entity, kGetWorldTM)(entity);
  const Framing& framing = close_framing ? kHead : kWholeFigure;
  const camera::Pose wanted = framing_pose(figure, framing);
  if (!shown_pose_valid) {
    shown_pose = wanted;
    shown_pose_valid = true;
  } else {
    shown_pose.x += (wanted.x - shown_pose.x) * kEase;
    shown_pose.y += (wanted.y - shown_pose.y) * kEase;
    shown_pose.z += (wanted.z - shown_pose.z) * kEase;
    shown_pose.yaw = ease_angle(shown_pose.yaw, wanted.yaw);
  }
  view.place(shown_pose, framing.fov);
}

// Ends the stage; the panels come back and the menu may turn to other pages again.
void end(void* menu, bool close_menu) {
  if (view.active()) {
    view.end();
  }
  close_framing = false;
  show_panels();
  action_hints::hide_menu_hints(false);
  native_ui::hold_page(-1);
  if (close_menu && menu && !menu_closed(menu) && menu_page(menu) == kPlayerPage) {
    slot<ApseClose>(menu, kApseClose)(menu, false);
  }
  phase = Phase::kIdle;
}

void begin(void* menu) {
  if (!menu || !stage_of(menu) || !player_page_of(menu) || !natives.open_context) {
    report("unavailable");
    return;
  }
  OpenContext context;
  context.value = read<std::uint64_t>(natives.open_context, 0);
  native_ui::hold_page(kPlayerPage);
  if (!slot<ApseOpen>(menu, kApseOpen)(menu, kPlayerPage, &context)) {
    native_ui::hold_page(-1);
    report("unavailable");
    return;
  }
  phase = Phase::kOpening;
  // Keys held when the stage goes up do not press anything.
  for (bool& down : keys_down) {
    down = true;
  }
}

std::string text_field(const sdk::Value& message, const char* name) {
  const sdk::Value* value = kcd2s::integration::script_field(message, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string();
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  return natives.services && natives.open_context && natives.stage_init &&
         natives.apse_player_vtable && natives.framework && natives.entity_system &&
         natives.spawn_params_init;
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  std::lock_guard<std::mutex> lock(request_mutex);
  if (operation == "creatorOpen") {
    open_requested = true;
    close_requested = false;
    return true;
  }
  if (operation == "creatorClose") {
    close_requested = true;
    open_requested = false;
    return true;
  }
  if (operation == "creatorFrame") {
    const sdk::Value* close = kcd2s::integration::script_field(message, "close");
    requested_frame = close && close->tag() == sdk::Tag::Bool && close->as_bool();
    return true;
  }
  if (operation == "creatorLook") {
    Appearance next;
    next.gender = text_field(message, "gender");
    next.body = text_field(message, "body");
    next.head = text_field(message, "head");
    next.hair = text_field(message, "hair");
    next.beard = text_field(message, "beard");
    requested_look = next;
    return true;
  }
  return false;
}

void frame(bool in_world) {
  bool open = false;
  bool close = false;
  {
    std::lock_guard<std::mutex> lock(request_mutex);
    open = open_requested;
    close = close_requested;
    open_requested = false;
    close_requested = false;
    if (requested_look) {
      look = *requested_look;
      look_changed = true;
      requested_look.reset();
    }
    if (requested_frame) {
      close_framing = *requested_frame;
      requested_frame.reset();
    }
  }
  void* menu = in_world ? apse() : nullptr;
  void* stage = menu ? stage_of(menu) : nullptr;
  if (void* figure = stage ? read<void*>(stage, kStageDummy) : nullptr) {
    // The inventory's figure too, not only the creator's.
    appearance::components::keep_doll_base(figure);
  }
  if (close && phase != Phase::kIdle) {
    end(menu, true);
  }
  if (open) {
    if (phase != Phase::kIdle) {
      end(menu, false);
    }
    if (!in_world) {
      report("unavailable");
    } else {
      begin(menu);
    }
    return;
  }
  if (phase == Phase::kIdle) {
    return;
  }
  if (!in_world || !menu) {
    // The camera went with the world.
    view = camera::AnchorView("kcds_creator_camera");
    end(nullptr, false);
    report("interrupted");
    return;
  }
  if (menu_closed(menu) || menu_page(menu) != kPlayerPage) {
    if (phase == Phase::kOpen) {
      end(menu, false);
      report("left");
    }
    return;
  }
  hide_panels(menu);
  action_hints::hide_menu_hints(true);
  if (phase == Phase::kOpening) {
    phase = Phase::kOpen;
    look_changed = true;
    report("open");
  }
  if (look_changed) {
    look_changed = false;
    dress_dummy(menu);
  }
  film(menu);
  read_keys();
}

void pump(const void* session, const RuntimeSender& send) {
  (void)session;
  std::vector<sdk::Value> taken;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    taken.swap(changes);
  }
  for (const sdk::Value& message : taken) {
    send(message);
  }
}

bool active() {
  return phase != Phase::kIdle;
}

}  // namespace kcds::runtime::character_creator
