#include "creator_stage.hpp"

#include <cctype>
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
#include "../appearance/outfit.hpp"
#include "../appearance/player_skeleton.hpp"
#include "../camera/anchor_view.hpp"
#include "../level/level_edits.hpp"
#include "../physics/local_world.hpp"
#include "../props/prop_entity.hpp"
#include "../core/game_layouts.hpp"
#include "../native_ui/native_ui.hpp"
#include "kcd2s/integration/appearance.hpp"
#include "kcd2s/integration/guid.hpp"
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

// At most this many pieces previewed at once (CharacterCreator.previewClothes).
constexpr std::size_t kMostClothes = 32;
constexpr unsigned kWearFrames = 5;
// The pause scene around the figure (CharacterCreator.open showroom: false): its geometry within
// kShowroomReach is hidden (the gilded framework, the page plates, the beams, the horse and dog
// figures) but the figure, its backdrop and light (kShowroomKept, kShowroomKeptNames) and the
// pedestal ("apsida_base", the rotating panel) unless pedestal: false. Its panel brushes are
// hidden with IRenderNode::Hide.
constexpr float kShowroomReach = 25.0f;
constexpr std::size_t kShowroomMost = 2000;
constexpr const char* kShowroomKept[] = {"Light",          "EnvironmentLight",    "FogVolume",
                                         "TagPoint",       "CameraSource",        "ParticleEffect",
                                         "UIApseLinkNode", "InventoryDummyPlayer"};
// The scene's own backdrop and air: its environment box (the dark room behind the figure) and the
// light shafts, which stay too.
constexpr const char* kShowroomKeptNames[] = {"ui_enviro", "ui3d_lightshafts", "kcds"};
// The scene's brushes that are its panels; its other brushes (the designer floor) stay.
constexpr char kShowroomPanels[] = "ui/panels/";
constexpr std::size_t kBrushHide = 68;
using BrushHide = void(__fastcall*)(void* brush, bool hide);

enum class Phase { kIdle, kOpening, kOpen };

// CEntity: the actor's entity (actor + 56), its world matrix (vtable byte offset 256) and
// SetWorldTM(matrix, reason) (+ 240).
constexpr std::size_t kActorEntity = kcds::layouts::Actor::kEntity;
constexpr std::size_t kGetWorldTM = 256;
constexpr std::size_t kSetWorldTM = 240;
// How far below the scene the hidden plates wait while the stage is up (move_out).
constexpr float kOutOfReach = 1000.0f;
// Where the pedestal's pieces are looked for around the figure's feet, metres, and at most how
// many entities.
constexpr float kPedestalReach = 1.5f;
constexpr std::size_t kPedestalMost = 64;
using SetWorldTM = void(__fastcall*)(void* entity, const float* matrix, unsigned reason);
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
std::optional<double> requested_turn;
std::optional<double> requested_orbit;
bool requested_showroom = true;
bool requested_pedestal = true;
bool requested_inventory = false;
// The item classes to preview on the figure (CharacterCreator.previewClothes), when changed.
std::optional<std::vector<kcd2s::integration::Guid>> requested_clothes;

// Engine thread only.
Phase phase = Phase::kIdle;
Appearance look;
bool look_changed = false;
std::vector<void*> hidden;
// The plates moved out of the pointer's reach, with their places.
struct MovedPlate {
  void* entity = nullptr;
  float matrix[12] = {};
};
std::vector<MovedPlate> moved_plates;
// The stage panel moved out of reach, and what hangs from it pinned where it stood.
bool panel_parked = false;
std::vector<MovedPlate> pinned;
// The showroom: whether this opening keeps it, and what of it is hidden.
bool keep_showroom = true;
bool keep_pedestal = true;
// The stage's panel (the paper doll's slot grid with the pedestal's shields) left drawn
// (CharacterCreator.open inventory: true); otherwise it is parked out of reach.
bool keep_inventory = false;
bool showroom_hidden = false;
std::vector<void*> showroom_entities;
std::vector<void*> showroom_brushes;
// The clothes previewed on the figure: what was asked, and the pieces made in its inventory and
// put on (taken off and out again when the preview changes or the stage ends).
std::vector<kcd2s::integration::Guid> clothes;
bool clothes_changed = false;
std::vector<void*> clothes_worn;
// The pieces still to put on, and the frames they were tried in.
std::vector<kcd2s::integration::Guid> clothes_pending;
unsigned clothes_tries = 0;
camera::AnchorView view("kcds_creator_camera");
bool close_framing = false;
camera::Pose shown_pose;
bool shown_pose_valid = false;
// The figure as the stage put it (its world matrix when the stage went up): the camera frames
// it from there, so turning the figure leaves the camera still. A script turns the figure about
// its feet (figure_turn, degrees from the stage's facing) and moves the camera around it
// (camera_orbit, degrees, 0 in front).
float stage_matrix[12] = {};
bool stage_matrix_valid = false;
double figure_turn = 0;
double camera_orbit = 0;
bool figure_turned = false;

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
  // The stage's own panel (+ kStagePanel) is the rotating pedestal the figure stands on, with the
  // shields on its front: it stays drawn and in place (the figure hangs from it).
  for (void* entity : {read<void*>(page, kPlayerListPlate), read<void*>(page, kPlayerInfoPlate)}) {
    if (entity) {
      panels.push_back(entity);
    }
  }
  return panels;
}

// A plate undrawn still takes the mouse: the pause screen finds what the pointer is over in 3D,
// and its paper-doll slots showed their tooltips over the figure. The plates are moved out of
// reach, kOutOfReach metres down, while the stage is up, and back after.
void move_out(void* entity) {
  const float* matrix = slot<const float*(__fastcall*)(void*)>(entity, kGetWorldTM)(entity);
  if (!matrix) {
    return;
  }
  MovedPlate plate;
  plate.entity = entity;
  std::memcpy(plate.matrix, matrix, sizeof(plate.matrix));
  float away[12];
  std::memcpy(away, matrix, sizeof(away));
  away[11] -= kOutOfReach;
  slot<SetWorldTM>(entity, kSetWorldTM)(entity, away, 0);
  moved_plates.push_back(plate);
}

void move_back() {
  for (const MovedPlate& plate : moved_plates) {
    slot<SetWorldTM>(plate.entity, kSetWorldTM)(plate.entity, plate.matrix, 0);
  }
  moved_plates.clear();
}

// The stage's own panel (+ kStagePanel), the rotating pedestal with the paper doll's slot grid,
// is undrawn as the plates are, and its slots took the pointer too ("Legs - Shoes" over the
// figure). It is moved out of reach as well, but the figure and the pedestal's pieces hang from
// it: they are pinned where they stood and put there again every frame (keep_pinned), and when
// the panel is back (unpark_stage_panel).
void park_stage_panel(void* menu) {
  if (panel_parked) {
    return;
  }
  void* stage = stage_of(menu);
  void* panel = stage ? read<void*>(stage, kStagePanel) : nullptr;
  void* dummy = stage ? read<void*>(stage, kStageDummy) : nullptr;
  void* figure = dummy ? read<void*>(dummy, kActorEntity) : nullptr;
  if (!panel || !figure) {
    return;
  }
  const float* placed = slot<const float*(__fastcall*)(void*)>(figure, kGetWorldTM)(figure);
  if (!placed) {
    return;
  }
  if (!stage_matrix_valid) {
    std::memcpy(stage_matrix, placed, sizeof(stage_matrix));
    stage_matrix_valid = true;
  }
  const float feet[3] = {stage_matrix[3], stage_matrix[7], stage_matrix[11]};
  for (const auto& found : physics::level_entities_near(feet, kPedestalReach, kPedestalMost)) {
    const std::string& name = found.described.name;
    const bool piece = name.find("apsida_base") != std::string::npos ||
                       name.find("rotating_panel_movingpart") != std::string::npos;
    void* entity = piece ? props::entity::by_id(found.described.entity_id) : nullptr;
    const float* matrix = entity && entity != panel
                              ? slot<const float*(__fastcall*)(void*)>(entity, kGetWorldTM)(entity)
                              : nullptr;
    if (matrix) {
      MovedPlate pin;
      pin.entity = entity;
      std::memcpy(pin.matrix, matrix, sizeof(pin.matrix));
      pinned.push_back(pin);
    }
  }
  if (!slot<IsInvisible>(panel, kIsInvisibleSlot * sizeof(void*))(panel)) {
    slot<SetInvisible>(panel, kSetInvisibleSlot * sizeof(void*))(panel, true, false);
    hidden.push_back(panel);
  }
  move_out(panel);
  panel_parked = true;
}

void keep_pinned() {
  for (const MovedPlate& pin : pinned) {
    slot<SetWorldTM>(pin.entity, kSetWorldTM)(pin.entity, pin.matrix, 0);
  }
}

// Keeps the panels undrawn and out of the pointer's reach; remembers only those the game drew,
// to draw them again.
void hide_panels(void* menu) {
  // Only with the showroom hidden: the gilded framework hangs from the panel too, and in the full
  // showroom it must stay where it stands.
  if (!keep_inventory && !keep_showroom) {
    park_stage_panel(menu);
  }
  keep_pinned();
  for (void* entity : panels_of(menu)) {
    if (slot<IsInvisible>(entity, kIsInvisibleSlot * sizeof(void*))(entity)) {
      continue;
    }
    slot<SetInvisible>(entity, kSetInvisibleSlot * sizeof(void*))(entity, true, false);
    hidden.push_back(entity);
    move_out(entity);
  }
}

void show_panels() {
  for (void* entity : hidden) {
    slot<SetInvisible>(entity, kSetInvisibleSlot * sizeof(void*))(entity, false, false);
  }
  hidden.clear();
}

// Whether a scene entity stays with the showroom hidden.
bool showroom_kept(const std::string& entity_class, const std::string& name) {
  for (const char* kept : kShowroomKept) {
    if (entity_class == kept) {
      return true;
    }
  }
  for (const char* kept : kShowroomKeptNames) {
    if (name.rfind(kept, 0) == 0) {
      return true;
    }
  }
  const bool pedestal = name.find("apsida_base") != std::string::npos ||
                        name.find("rotating_panel") != std::string::npos;
  return pedestal && (keep_pedestal || keep_inventory);
}

// Whether a brush is one of the scene's panels (its meshes are written in either case).
bool panel_brush(const std::string& mesh) {
  std::string lower = mesh;
  for (char& c : lower) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower.find(kShowroomPanels) != std::string::npos;
}

// Hides the showroom around the figure standing at `figure` (its entity `own`); only what was
// drawn, so only that is drawn again.
void hide_showroom(const float* figure, void* own) {
  showroom_hidden = true;
  const float feet[3] = {figure[3], figure[7], figure[11]};
  for (const auto& found : physics::level_entities_near(feet, kShowroomReach, kShowroomMost)) {
    if (showroom_kept(found.described.entity_class, found.described.name)) {
      continue;
    }
    void* entity = props::entity::by_id(found.described.entity_id);
    if (!entity || entity == own ||
        slot<IsInvisible>(entity, kIsInvisibleSlot * sizeof(void*))(entity)) {
      continue;
    }
    slot<SetInvisible>(entity, kSetInvisibleSlot * sizeof(void*))(entity, true, false);
    showroom_entities.push_back(entity);
    // The page plates (UIPlayerList, UIInventoryInfo, ...) take the pointer too.
    if (found.described.name.rfind("UI", 0) == 0) {
      move_out(entity);
    }
  }
  for (const auto& brush : level::brushes_near(feet, kShowroomReach)) {
    if (!brush.brush || brush.hidden || !panel_brush(brush.mesh)) {
      continue;
    }
    slot<BrushHide>(brush.brush, kBrushHide * sizeof(void*))(brush.brush, true);
    showroom_brushes.push_back(brush.brush);
  }
  if (log_line) {
    log_line("CREATOR showroom hidden: %zu pieces, %zu panels", showroom_entities.size(),
             showroom_brushes.size());
  }
}

void show_showroom() {
  for (void* entity : showroom_entities) {
    slot<SetInvisible>(entity, kSetInvisibleSlot * sizeof(void*))(entity, false, false);
  }
  for (void* brush : showroom_brushes) {
    slot<BrushHide>(brush, kBrushHide * sizeof(void*))(brush, false);
  }
  showroom_entities.clear();
  showroom_brushes.clear();
  showroom_hidden = false;
}

// The figure's actor and its entity, or nulls.
void* figure_actor(void* menu) {
  void* stage = menu ? stage_of(menu) : nullptr;
  return stage ? read<void*>(stage, kStageDummy) : nullptr;
}

// Takes the previewed pieces off the figure and out of its inventory.
void undress_clothes(void* menu) {
  void* dummy = figure_actor(menu);
  void* entity = dummy ? read<void*>(dummy, kActorEntity) : nullptr;
  for (void* item : clothes_worn) {
    if (dummy && entity) {
      appearance::outfit::wear(dummy, item, false);
      appearance::outfit::remove_item_from_inventory(entity, item);
    }
  }
  clothes_worn.clear();
}

// Puts the previewed pieces on the figure through the game's own equipment, as a replica wears
// its owner's: made in the figure's inventory, never the player's.
// Several pieces put on in one frame: the game refused some that it takes alone, so a refused
// piece is tried again on the next frames, kWearFrames in all (a woman's piece on a man's figure,
// or a horse's, stays refused).
void dress_clothes(void* menu) {
  undress_clothes(menu);
  clothes_pending = clothes;
  clothes_tries = 0;
}

void wear_pending(void* menu) {
  if (clothes_pending.empty()) {
    return;
  }
  void* dummy = figure_actor(menu);
  void* entity = dummy ? read<void*>(dummy, kActorEntity) : nullptr;
  if (!dummy || !entity) {
    return;
  }
  std::vector<kcd2s::integration::Guid> refused;
  for (const auto& item_class : clothes_pending) {
    void* item = appearance::outfit::create_item_in_inventory(entity, item_class);
    if (item && appearance::outfit::wear(dummy, item, true)) {
      clothes_worn.push_back(item);
      continue;
    }
    if (item) {
      appearance::outfit::remove_item_from_inventory(entity, item);
    }
    refused.push_back(item_class);
  }
  clothes_pending = std::move(refused);
  if (++clothes_tries >= kWearFrames || clothes_pending.empty()) {
    if (log_line) {
      log_line("CREATOR clothes previewed: %zu on, %zu refused", clothes_worn.size(),
               clothes_pending.size());
    }
    clothes_pending.clear();
  }
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

// A world matrix (3x4 by rows) turned `degrees` about the world's up axis through its own place.
void turned(const float* matrix, double degrees, float* out) {
  const double angle = degrees * kPi / 180.0;
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  for (int column = 0; column < 3; ++column) {
    const double x = matrix[column];
    const double y = matrix[4 + column];
    out[column] = static_cast<float>(c * x - s * y);
    out[4 + column] = static_cast<float>(s * x + c * y);
    out[8 + column] = matrix[8 + column];
  }
  out[3] = matrix[3];
  out[7] = matrix[7];
  out[11] = matrix[11];
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
  if (!stage_matrix_valid) {
    std::memcpy(stage_matrix, figure, sizeof(stage_matrix));
    stage_matrix_valid = true;
  }
  if (!keep_showroom && !showroom_hidden) {
    hide_showroom(stage_matrix, entity);
  }
  // The figure turned by the script, again every frame (the stage may put it back), and where it
  // stood while its pedestal panel is out of reach.
  if (panel_parked || figure_turn != 0 || figure_turned) {
    float matrix[12];
    turned(stage_matrix, figure_turn, matrix);
    slot<SetWorldTM>(entity, kSetWorldTM)(entity, matrix, 0);
    figure_turned = figure_turn != 0;
  }
  // The camera frames the stage's figure, moved around it by the orbit.
  float framed[12];
  turned(stage_matrix, camera_orbit, framed);
  const Framing& framing = close_framing ? kHead : kWholeFigure;
  const camera::Pose wanted = framing_pose(framed, framing);
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
  // The panels back in place first: the figure and the pedestal's pieces hang from the stage's
  // panel, and are put back where they stood after it.
  move_back();
  keep_pinned();
  // The figure faces as the stage put it again (the inventory shows the same figure).
  void* stage = menu ? stage_of(menu) : nullptr;
  void* dummy = stage ? read<void*>(stage, kStageDummy) : nullptr;
  void* entity = dummy ? read<void*>(dummy, kActorEntity) : nullptr;
  if (entity && stage_matrix_valid && (figure_turned || panel_parked)) {
    slot<SetWorldTM>(entity, kSetWorldTM)(entity, stage_matrix, 0);
  }
  pinned.clear();
  panel_parked = false;
  stage_matrix_valid = false;
  figure_turned = false;
  figure_turn = 0;
  camera_orbit = 0;
  close_framing = false;
  show_panels();
  show_showroom();
  undress_clothes(menu);
  clothes.clear();
  clothes_pending.clear();
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
    const sdk::Value* showroom = kcd2s::integration::script_field(message, "showroom");
    requested_showroom = !showroom || showroom->tag() != sdk::Tag::Bool || showroom->as_bool();
    const sdk::Value* pedestal = kcd2s::integration::script_field(message, "pedestal");
    requested_pedestal = !pedestal || pedestal->tag() != sdk::Tag::Bool || pedestal->as_bool();
    const sdk::Value* inventory = kcd2s::integration::script_field(message, "inventory");
    requested_inventory = inventory && inventory->tag() == sdk::Tag::Bool && inventory->as_bool();
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
  if (operation == "creatorTurn" || operation == "creatorOrbit") {
    const sdk::Value* degrees = kcd2s::integration::script_field(message, "degrees");
    double value = 0;
    if (degrees && degrees->tag() == sdk::Tag::Double) {
      value = degrees->as_double();
    } else if (degrees && degrees->tag() == sdk::Tag::Int) {
      value = static_cast<double>(degrees->as_int());
    } else {
      return true;
    }
    if (!std::isfinite(value)) {
      return true;
    }
    const double angle = std::fmod(value, 360.0);
    (operation == "creatorTurn" ? requested_turn : requested_orbit) = angle;
    return true;
  }
  if (operation == "creatorClothes") {
    const sdk::Value* items = kcd2s::integration::script_field(message, "items");
    std::vector<kcd2s::integration::Guid> wanted;
    if (items && items->tag() == sdk::Tag::Array) {
      for (const sdk::Value& item : items->as_array()) {
        kcd2s::integration::Guid id{};
        if (item.tag() == sdk::Tag::Str && kcd2s::integration::parse_guid(item.as_str(), id) &&
            wanted.size() < kMostClothes) {
          wanted.push_back(id);
        }
      }
    }
    requested_clothes = std::move(wanted);
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
    if (open) {
      keep_showroom = requested_showroom;
      keep_pedestal = requested_pedestal;
      keep_inventory = requested_inventory;
    }
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
    if (requested_turn) {
      figure_turn = *requested_turn;
      requested_turn.reset();
    }
    if (requested_orbit) {
      camera_orbit = *requested_orbit;
      requested_orbit.reset();
    }
    if (requested_clothes) {
      clothes = std::move(*requested_clothes);
      clothes_changed = true;
      requested_clothes.reset();
    }
  }
  void* menu = in_world ? apse() : nullptr;
  void* stage = menu ? stage_of(menu) : nullptr;
  if (void* figure = stage ? read<void*>(stage, kStageDummy) : nullptr) {
    // A woman's figure stands on her skeleton, as her body does.
    appearance::player_skeleton::keep_figure(
        figure, appearance::components::worn(figure).gender == "female");
    appearance::player_skeleton::settle_figure(figure);
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
    // A new body takes what it wore off: the preview goes on again.
    clothes_changed = clothes_changed || !clothes.empty();
  }
  if (clothes_changed) {
    clothes_changed = false;
    dress_clothes(menu);
  }
  wear_pending(menu);
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
