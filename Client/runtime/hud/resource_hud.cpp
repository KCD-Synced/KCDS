#include "resource_hud.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../native_ui/native_ui.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::hud {
namespace {

namespace sdk = kcd2s::sdk;

// Game services: the tutorial manager (+ 296, then + 32) and the tutorial display (+ 224, then
// + 376; its vtable slot 18 hides the current tutorial).
constexpr std::size_t kServicesTutorials = 296;
constexpr std::size_t kTutorialManager = 32;
constexpr std::size_t kServicesPlayer = 224;
// The game's UI state registry (services + 216: slot 0 sets a state,
// slot 1 clears it). The game's own shooting contest table enters state 27,
// MinigameArchery (C_UIShootingContest's Show), without which its HUD table stays empty.
constexpr std::size_t kServicesUiStates = 216;
constexpr unsigned kArcheryUiState = 27;
constexpr std::size_t kTutorialDisplay = 376;
constexpr std::size_t kHideCurrentTutorialSlot = 18;
// I3DEngine::SetPostEffectParam(name, value, force), vtable slot 171.
constexpr std::size_t kSetPostEffectParamSlot = 171;
// The HUD element's one instance.
constexpr int kHudInstance = 0;

using TutorialCall = char(__fastcall*)(void* manager, void* id);
using StringCall = void*(__fastcall*)(void* string);
using SetPostEffectParam = void(__fastcall*)(void* engine, const char* name, float value,
                                             bool force);

// A resource's Hud call waiting for the engine thread.
struct Call {
  std::string name;
  sdk::Value::Array args;
};

Natives natives;
std::mutex queue_mutex;
std::vector<Call> queued;
// Calls waiting for the engine thread; a flood from a script is cut off.
constexpr std::size_t kMaxQueued = 256;

// HUD elements a resource hides, and those whose last hold went since the engine thread looked.
std::set<std::string> hidden;
std::set<std::string> released;

// The HUD movie clips a resource may hide (Libs/UI/UIElements/HUD.xml <MovieClips>).
const std::set<std::string> kElements = {
    "Compass",          "Stats",         "QAMWeapon",    "QAMFood",         "Subtitles",
    "InfoText",         "GameLog",       "Hints",        "DialogLeft",      "DialogRight",
    "Cursor",           "Crime",         "Wanted",       "PopUpBackground", "TutorialMessage",
    "FancyEvent",       "SkillCheck",    "ItemTransfer", "Buffs",           "CommonEvent",
    "DiceCursor",       "Trespassing",   "RatioStrips",  "ShootingContest", "Bubbles",
    "TutorialInDialog", "DiceContainer", "Vignette",
};

// The game sets its clips' visibility itself, so a hidden clip is hidden again every frame. The
// visibility it had before the first hide is kept and given back on release (engine thread).
std::map<std::string, bool> shown_before;

// The server's switch for the game's combat marker (CombatMarker.setEnabled): the
// crossed swords under the compass are the HUD's Crime clip, hidden like a resource's hold until
// the server shows it again or the session ends (guarded by queue_mutex).
const std::string kCombatMarkerClip = "Crime";
bool combat_marker_hidden = false;

// The same for the game's restricted-area marker (RestrictedAreaMarker.setEnabled): the
// warning that a player is trespassing is the HUD's Trespassing clip.
const std::string kRestrictedAreaMarkerClip = "Trespassing";
bool restricted_area_marker_hidden = false;
// The server allows this player in private areas.
bool trespass_allowed = false;
// While the local player lies knocked out, the HUD is hidden: on the dimmed view its health and
// stamina bar read as a loading bar, and the compass, effects and quick slots over it
// were "shit" ("total hide UI in K.O."). The vignette stays: it is part of
// the dimmed view.
const std::string kVignetteClip = "Vignette";
bool knocked_out = false;

void set_hidden(std::set<std::string> elements) {
  for (const std::string& element : hidden) {
    if (!elements.count(element)) {
      released.insert(element);
    }
  }
  for (const std::string& element : elements) {
    released.erase(element);
  }
  hidden = std::move(elements);
}

// The engine post-effect parameters screen effects drive (Client/resources/screen_effects.cjs),
// with the latest combined value of each not yet applied.
const std::set<std::string> kScreenParameters = {
    "Global_User_Saturation", "Global_User_Brightness", "Global_User_Contrast",
    "Global_User_ColorC",     "Global_User_ColorM",     "Global_User_ColorY",
    "Global_User_ColorK",     "Global_User_ColorHue",   "FilterBlurring_Amount",
    "ImageGhosting_Amount",
};
std::map<std::string, double> screen_pending;

// The view camera, written by the engine thread and streamed once a resource wants it.
std::mutex camera_mutex;
bool camera_valid = false;
std::array<float, 13> camera_now{};
bool camera_wanted = false;
bool camera_reported_valid = false;
std::array<float, 13> camera_reported{};

const void* reported_session = nullptr;
bool reported_ready = false;
bool reported_dialogue = false;

// The HUD movie's method of a resource's message call, and how many arguments it takes.
struct Method {
  const char* call;
  const char* method;
  std::size_t count;
};
constexpr Method kMethods[] = {
    {"showNotification", "ShowNotification", 1},
    {"showBanner", "ShowInfoText", 4},  // text, priority, durationMs, background
    {"hideBanner", "HideInfoText", 0},
    {"notifyPerkUse", "ShowPerkUsed", 2},
    {"notifyPerk", "ShowPerkGained", 2},
    {"notifyCombo", "ShowComboLearned", 2},
    {"notifyReputation", "ShowReputationChanged", 2},
    {"notifyEventResult", "ShowRandomEventResult", 3},
    {"notifyXp", "ShowLevelXpGain", 3},
    {"hideCodexHint", "HideCodexActionHint", 0},
    {"clearNotifications", "ClearNotificationQueue", 0},
    // The game's shooting contest tables (its HUD movie, Libs/UI/UIElements/HUD.xml).
    {"showShootingContest", "ShowShootingContestTable", 7},
    {"updateShootingContest", "UpdateShootingContestTable", 6},
    {"hideShootingContest", "HideShootingContestTable", 0},
    {"showShootingHit", "ShowShootingHit", 2},
    {"showHorseContest", "ShowHorseShootingContestTable", 5},
    {"updateHorseContest", "UpdateHorseShootingContestTable", 1},
    {"hideHorseContest", "HideHorseShootingContestTable", 0},
};

const Method* method_of(const std::string& call) {
  for (const Method& method : kMethods) {
    if (call == method.call) {
      return &method;
    }
  }
  return nullptr;
}

bool is_tutorial_call(const std::string& call, std::size_t count) {
  if (call == "showTutorial" || call == "hideTutorial") {
    return count == 1;
  }
  return call == "closeTutorial" && count == 0;
}

sdk::Value hud_request(const char* op, sdk::Value::Map fields) {
  fields.emplace_back("op", sdk::Value(std::string(op)));
  fields.emplace_back("name", sdk::Value(std::string("hud")));
  fields.emplace_back("instance", sdk::Value(static_cast<std::int64_t>(kHudInstance)));
  return native_ui::request(sdk::Value(std::move(fields)));
}

void call_method(const Method& method, const sdk::Value::Array& args) {
  hud_request("elementCall",
              {{"fn", sdk::Value(std::string(method.method))}, {"args", sdk::Value(args)}});
}

template <class T>
T read(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

void* services() {
  return natives.services ? natives.services() : nullptr;
}

void* tutorial_manager() {
  void* game = services();
  void* tutorials = game ? read<void*>(game, kServicesTutorials) : nullptr;
  return tutorials ? read<void*>(tutorials, kTutorialManager) : nullptr;
}

void* tutorial_display() {
  void* game = services();
  void* player = game ? read<void*>(game, kServicesPlayer) : nullptr;
  return player ? read<void*>(player, kTutorialDisplay) : nullptr;
}

// A tutorial native with the tutorial's id as the game's own string.
void with_id(void* function, void* manager, const std::string& id) {
  alignas(8) std::uint8_t string[8] = {};
  reinterpret_cast<StringCall>(natives.string_init)(string);
  natives.string_assign(string, id.c_str());
  reinterpret_cast<TutorialCall>(function)(manager, string);
  reinterpret_cast<StringCall>(natives.string_free)(string);
}

// The game's tutorials by id (Game.ShowTutorial and the others). The game shows a tutorial once
// and then keeps it as seen, so showTutorial resets it first: without the reset a resource's
// second showTutorial of it showed nothing.
void call_tutorial(const std::string& call, const sdk::Value::Array& args) {
  if (call == "closeTutorial") {
    void* display = tutorial_display();
    if (display) {
      using Hide = void(__fastcall*)(void*);
      reinterpret_cast<Hide>((*reinterpret_cast<void***>(display))[kHideCurrentTutorialSlot])(
          display);
    }
    return;
  }
  void* manager = tutorial_manager();
  if (!manager || args.empty() || args[0].tag() != sdk::Tag::Str) {
    return;
  }
  const std::string& id = args[0].as_str();
  if (call == "showTutorial") {
    with_id(natives.tutorial_reset, manager, id);
    with_id(natives.tutorial_show, manager, id);
  } else {
    with_id(natives.tutorial_hide, nullptr, id);
  }
}

// Sets or clears one of the game's UI states.
void ui_state(unsigned state, bool on) {
  void* game = services();
  void* registry = game ? read<void*>(game, kServicesUiStates) : nullptr;
  if (!registry) {
    return;
  }
  using SetState = void(__fastcall*)(void*, std::uint8_t);
  auto** table = *reinterpret_cast<SetState**>(registry);
  table[on ? 0 : 1](registry, static_cast<std::uint8_t>(state));
}

// The shooting contest tables show only in the game's archery UI state: entered with a table,
// left when it is hidden.
void contest_state(const std::string& call) {
  if (call == "showShootingContest" || call == "showHorseContest") {
    ui_state(kArcheryUiState, true);
  } else if (call == "hideShootingContest" || call == "hideHorseContest") {
    ui_state(kArcheryUiState, false);
  }
}

void run(const Call& call) {
  if (const Method* method = method_of(call.name)) {
    contest_state(call.name);
    call_method(*method, call.args);
    return;
  }
  if (natives.allow_tutorials) {
    natives.allow_tutorials(true);
  }
  call_tutorial(call.name, call.args);
  if (natives.allow_tutorials) {
    natives.allow_tutorials(false);
  }
}

// Hides the hidden clips again and gives the released ones the visibility they had.
void keep_clips(const std::set<std::string>& hide, const std::set<std::string>& release) {
  for (const std::string& element : release) {
    auto it = shown_before.find(element);
    if (it == shown_before.end()) {
      continue;
    }
    hud_request("elementSetClip",
                {{"clip", sdk::Value(element)}, {"visible", sdk::Value(it->second)}});
    shown_before.erase(it);
  }
  for (const std::string& element : hide) {
    if (!shown_before.count(element)) {
      const sdk::Value clip = hud_request("elementGetClip", {{"clip", sdk::Value(element)}});
      const sdk::Value* visible = kcd2s::integration::script_field(clip, "visible");
      if (!visible || visible->tag() != sdk::Tag::Bool) {
        continue;
      }
      shown_before[element] = visible->as_bool();
    }
    hud_request("elementSetClip", {{"clip", sdk::Value(element)}, {"visible", sdk::Value(false)}});
  }
}

// The engine's post-effect parameters (System.SetScreenFx).
void set_screen(const std::map<std::string, double>& values) {
  void* engine = natives.engine_3d ? *natives.engine_3d : nullptr;
  if (!engine) {
    return;
  }
  auto set = reinterpret_cast<SetPostEffectParam>(
      (*reinterpret_cast<void***>(engine))[kSetPostEffectParamSlot]);
  for (const auto& [name, value] : values) {
    set(engine, name.c_str(), static_cast<float>(value), false);
  }
}

// Sends the view camera when a resource wants it and it changed (network thread).
void pump_camera(const RuntimeSender& send) {
  std::array<float, 13> camera;
  bool valid;
  {
    std::lock_guard<std::mutex> lock(camera_mutex);
    if (!camera_wanted) {
      return;
    }
    camera = camera_now;
    valid = camera_valid;
    if (valid == camera_reported_valid && (!valid || camera == camera_reported)) {
      return;
    }
  }
  sdk::Value::Map message{{"op", sdk::Value(std::string("camera"))}};
  if (valid) {
    sdk::Value::Array matrix;
    for (int index = 0; index < 12; ++index) {
      matrix.push_back(sdk::Value(static_cast<double>(camera[index])));
    }
    message.emplace_back("m", sdk::Value(std::move(matrix)));
    message.emplace_back("fov", sdk::Value(static_cast<double>(camera[12])));
  }
  if (send(sdk::Value(std::move(message)))) {
    std::lock_guard<std::mutex> lock(camera_mutex);
    camera_reported_valid = valid;
    camera_reported = camera;
  }
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  if (operation == "hudHidden") {
    std::set<std::string> elements;
    const sdk::Value* list = kcd2s::integration::script_field(message, "elements");
    if (list && list->tag() == sdk::Tag::Array) {
      for (const sdk::Value& value : list->as_array()) {
        if (value.tag() == sdk::Tag::Str && kElements.count(value.as_str())) {
          elements.insert(value.as_str());
        }
      }
    }
    std::lock_guard<std::mutex> lock(queue_mutex);
    set_hidden(std::move(elements));
    return true;
  }
  if (operation == "cameraWanted") {
    std::lock_guard<std::mutex> lock(camera_mutex);
    camera_wanted = true;
    return true;
  }
  if (operation == "screenFx") {
    const sdk::Value* values = kcd2s::integration::script_field(message, "values");
    if (!values || values->tag() != sdk::Tag::Map) {
      return true;
    }
    std::lock_guard<std::mutex> lock(queue_mutex);
    for (const auto& [name, value] : values->as_map()) {
      if (!kScreenParameters.count(name)) {
        continue;
      }
      if (value.tag() == sdk::Tag::Double) {
        screen_pending[name] = value.as_double();
      } else if (value.tag() == sdk::Tag::Int) {
        screen_pending[name] = static_cast<double>(value.as_int());
      }
    }
    return true;
  }
  if (operation != "hud") {
    return false;
  }
  const sdk::Value* call = kcd2s::integration::script_field(message, "call");
  if (!call || call->tag() != sdk::Tag::Str) {
    return true;
  }
  sdk::Value::Array args;
  const sdk::Value* list = kcd2s::integration::script_field(message, "args");
  if (list && list->tag() == sdk::Tag::Array) {
    args = list->as_array();
  }
  const Method* method = method_of(call->as_str());
  bool known = false;
  if (method) {
    known = args.size() == method->count;
  } else {
    known = is_tutorial_call(call->as_str(), args.size());
  }
  if (!known) {
    return true;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back({call->as_str(), std::move(args)});
  }
  return true;
}

void pump(const void* session, const RuntimeSender& send, bool hud_ready, bool in_dialogue) {
  if (session != reported_session) {
    reported_session = session;
    reported_ready = false;
    reported_dialogue = false;
    {
      std::lock_guard<std::mutex> lock(camera_mutex);
      camera_wanted = false;
      camera_reported_valid = false;
    }
    // A new session's runtime starts without holds, and its server shows the combat and
    // restricted-area markers until it says otherwise.
    std::lock_guard<std::mutex> lock(queue_mutex);
    set_hidden({});
    combat_marker_hidden = false;
    restricted_area_marker_hidden = false;
    trespass_allowed = false;
  }
  pump_camera(send);
  if (hud_ready == reported_ready && in_dialogue == reported_dialogue) {
    return;
  }
  sdk::Value::Map host{{"op", sdk::Value(std::string("hudHost"))},
                       {"ready", sdk::Value(hud_ready)},
                       {"dialogue", sdk::Value(in_dialogue)}};
  if (send(sdk::Value(std::move(host)))) {
    reported_ready = hud_ready;
    reported_dialogue = in_dialogue;
  }
}

void set_combat_marker_shown(bool shown) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (combat_marker_hidden == !shown) {
    return;
  }
  combat_marker_hidden = !shown;
  if (shown && !hidden.count(kCombatMarkerClip)) {
    released.insert(kCombatMarkerClip);
  }
}

void set_knocked_out(bool down) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (knocked_out == down) {
    return;
  }
  knocked_out = down;
  if (!down) {
    // Given back, except what a resource or the server's markers still hide (frame keeps those).
    for (const std::string& element : kElements) {
      if (element != kVignetteClip && !hidden.count(element)) {
        released.insert(element);
      }
    }
  }
}

void set_restricted_area_marker_shown(bool shown) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (restricted_area_marker_hidden == !shown) {
    return;
  }
  restricted_area_marker_hidden = !shown;
  if (shown && !trespass_allowed && !hidden.count(kRestrictedAreaMarkerClip)) {
    released.insert(kRestrictedAreaMarkerClip);
  }
}

void set_trespass_allowed(bool allowed) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (trespass_allowed == allowed) {
    return;
  }
  trespass_allowed = allowed;
  if (!allowed && !restricted_area_marker_hidden && !hidden.count(kRestrictedAreaMarkerClip)) {
    released.insert(kRestrictedAreaMarkerClip);
  }
}

void set_camera(const float* matrix, float fov) {
  std::lock_guard<std::mutex> lock(camera_mutex);
  std::copy(matrix, matrix + 12, camera_now.begin());
  camera_now[12] = fov;
  camera_valid = true;
}

void clear_camera() {
  std::lock_guard<std::mutex> lock(camera_mutex);
  camera_valid = false;
}

bool install(const Natives& given) {
  natives = given;
  return natives.services && natives.string_init && natives.string_free && natives.string_assign &&
         natives.tutorial_show && natives.tutorial_reset && natives.tutorial_hide &&
         natives.engine_3d;
}

void frame(bool in_world) {
  std::vector<Call> calls;
  std::set<std::string> hide;
  std::set<std::string> release;
  std::map<std::string, double> screen;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    calls.swap(queued);
    if (!in_world) {
      // Outside a world the calls are dropped; screen effects wait for a world.
      shown_before.clear();
      return;
    }
    hide = hidden;
    release.swap(released);
    if (combat_marker_hidden) {
      hide.insert(kCombatMarkerClip);
      release.erase(kCombatMarkerClip);
    }
    if (restricted_area_marker_hidden || trespass_allowed) {
      hide.insert(kRestrictedAreaMarkerClip);
      release.erase(kRestrictedAreaMarkerClip);
    }
    if (knocked_out) {
      for (const std::string& element : kElements) {
        if (element != kVignetteClip) {
          hide.insert(element);
          release.erase(element);
        }
      }
    }
    screen.swap(screen_pending);
  }
  for (const Call& call : calls) {
    run(call);
  }
  keep_clips(hide, release);
  set_screen(screen);
}

}  // namespace kcds::runtime::hud
