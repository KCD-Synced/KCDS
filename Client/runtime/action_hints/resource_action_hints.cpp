#include "resource_action_hints.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <functional>

#include "MinHook.h"
#include "kcd2s/integration/script_channel.hpp"

#include "../branding/runtime_branding.hpp"
#include "hint_width.hpp"

namespace kcds::runtime::action_hints {
namespace {

namespace sdk = kcd2s::sdk;

// The game control behind each hint key: the superaction's action and action map that are
// active while the player walks around (Libs/Config/keybindSuperactions.xml). script_action is
// KCDS's own key for scripts: U, which the game binds only in its "haste" prompts;
// its hint is drawn as that action's (the game shows its key) and its press read from the key
// itself (scan code, the same physical key in every layout), as the haste map is not active.
// The HUD draws one hint per action: a hold hint is drawn as another action of the
// same superaction where it has one, so a press hint and a hold hint on one key both show.
struct Control {
  const char* key;
  const char* actionmap;
  const char* action;
  std::uint16_t scan = 0;
  const char* hold_actionmap = nullptr;
  const char* hold_action = nullptr;
};
constexpr Control kControls[] = {
    {"primary_action", "interaction", "use", 0, "interaction", "use_other"},
    {"attack_special", "player", "grab_body", 0, "interaction", "butcher"},
    {"torch", "draw_holster_torch", "toggle_torch"},
    {"companion_interaction", "interaction", "use_horse", 0, "call", "call"},
    {"distract", "crouch", "distract_init"},
    {"jump", "movement", "jump"},
    {"script_action", "haste", "haste_keyu", 0x16},
};

// The game's other actions on a control's key out in the world (the superaction primary_action:
// what E does while the player carries something, sits, faces a horse or a bed, ...). A script's
// hint takes the press under any of them while it is shown (carrying a sack, E only
// put it down and the hint never fired).
struct Alias {
  const char* key;
  const char* action;
};
constexpr Alias kAliases[] = {
    {"primary_action", "use_other"},
    {"primary_action", "talk"},
    {"primary_action", "trigger_use"},
    {"primary_action", "trigger_use_hold"},
    {"primary_action", "use_bed_sit"},
    {"primary_action", "use_bed_sleep"},
    {"primary_action", "pickpocketing"},
    {"primary_action", "put_corpse"},
    {"primary_action", "deposit_item"},
    {"primary_action", "put_item"},
    {"primary_action", "mount_horse"},
    // X's whistle for the horse starts on the press of call: a hint on X takes it.
    {"companion_interaction", "call"},
};

const Control* control_for(const std::string& key) {
  for (const Control& control : kControls) {
    if (key == control.key) {
      return &control;
    }
  }
  return nullptr;
}

// Whether a game action is the press of a control's key.
bool control_action(const Control& control, const char* action) {
  if (std::strcmp(control.action, action) == 0) {
    return true;
  }
  for (const Alias& alias : kAliases) {
    if (std::strcmp(alias.key, control.key) == 0 && std::strcmp(alias.action, action) == 0) {
      return true;
    }
  }
  return false;
}

// The script action key's state at the last frame (engine thread).
bool script_key_down = false;

// One physical press of a control raises several of the game's actions, its own and its aliases'
// (use, use_other and the others on E), each as a press: a hint fired three times for
// one press, and a hint that showed up in that press (a pillory's "Release" after its "Put in")
// fired with it. A control's first press is the press; the others until it comes up (mode 2) are
// that same press. A press the game never lets go of counts as new after kPressStaleMs.
struct ControlPress {
  bool down = false;
  ULONGLONG at = 0;
};
std::unordered_map<std::string, ControlPress> presses;  // by hint key (engine thread)
constexpr ULONGLONG kPressStaleMs = 700;

// Whether this action event is the control's own press (mode 1) or release (mode 2), not one more
// of the same.
bool first_of_press(const std::string& key, unsigned mode) {
  ControlPress& press = presses[key];
  const ULONGLONG now = GetTickCount64();
  if (mode == 1) {
    if (press.down && now - press.at < kPressStaleMs) {
      return false;
    }
    press = ControlPress{true, now};
    return true;
  }
  if (mode == 2) {
    if (!press.down) {
      return false;
    }
    press.down = false;
    return true;
  }
  return true;
}

// S_ActionHint (WHGame 1.5.5, 224 bytes, constructor): CryString action at +0,
// action map at +8, type byte at +20 (0 press, 2 hold; the HUD flags a hold from it),
// class byte at +21, CryString text at +32, byte +40 set
// when the text is a localization key, visible byte +41, hold duration float +48, enabled
// byte +64.
constexpr std::size_t kHintSize = 224;
constexpr std::size_t kAction = 0;
constexpr std::size_t kActionMap = 8;
constexpr std::size_t kType = 20;
constexpr std::size_t kClass = 21;
constexpr std::size_t kText = 32;
constexpr std::size_t kLocalized = 40;
constexpr std::size_t kVisible = 41;
constexpr std::size_t kHoldDuration = 48;
constexpr std::size_t kEnabled = 64;
constexpr std::uint8_t kTypePress = 0;
constexpr std::uint8_t kTypeHold = 2;
// The class byte is the hint's place on screen: 1 to 3 the pause screen's bottom
// left, centre and right, 4 to 6 its top left, centre (the page tabs) and right.
constexpr std::uint8_t kFirstMenuClass = 1;
constexpr std::uint8_t kLastMenuClass = 6;

struct Shown {
  std::int64_t id = 0;
  std::string key;
  std::string text;
  bool hold = false;
  double hold_seconds = 1;
  bool enabled = true;
  // An engine hint's: its text is a localization key, and firing it calls this on the engine
  // thread instead of reporting it to the runtime.
  bool localized = false;
  std::function<void()> on_fire;
};

// The std::vector<S_ActionHint*> the hint manager passes to the HUD listener.
struct HintList {
  void** begin;
  void** end;
  void** capacity;
};
using Listener = void(__fastcall*)(void* self, HintList* hints, unsigned flags);
using HintConstructor = void*(__fastcall*)(void* hint);
// The listener's second virtual: the HUD movie's SetProgress, the hold ring of the
// hint on that action, from progress 0..1 (sent as 0..100) and the hint's class.
using HoldProgress = double(__fastcall*)(void* listener, float progress, void* action,
                                         int hint_class);

Logger log_line = nullptr;
Listener original_listener = nullptr;
HintConstructor construct_hint = nullptr;
bool (*menu_open)() = nullptr;
StringAssign assign_string = nullptr;

std::mutex state_mutex;
std::vector<Shown> shown;
bool shown_changed = false;
std::vector<std::int64_t> fired;
// Hints the client's own features show (set_engine_hint), engine thread only.
std::vector<Shown> engine_shown;
bool engine_changed = false;

// Whether a script's enabled hint is on the key: it takes the key's press.
bool key_taken(const std::vector<Shown>& scripts, const std::string& key) {
  for (const Shown& entry : scripts) {
    if (entry.key == key && entry.enabled) {
      return true;
    }
  }
  return false;
}

// The resource hints and the engine hints, as drawn now. An engine hint on a key a script's hint
// takes is neither drawn nor pressed: the press is the script's, and the client's own
// Rob under a script's "Lead on a rope" only confused.
std::vector<Shown> current_hints() {
  std::vector<Shown> current;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    current = shown;
  }
  const std::vector<Shown> scripts = current;
  for (const Shown& engine : engine_shown) {
    if (!key_taken(scripts, engine.key)) {
      current.push_back(engine);
    }
  }
  return current;
}

// Engine thread only.
std::map<std::string, std::uint8_t*> native_hints;  // one S_ActionHint per control, kept
void* last_listener = nullptr;
unsigned last_flags = 0;
std::vector<void*> last_game_hints;
// While set, the pause screen's own hints are not drawn.
bool menu_hints_hidden = false;
// While set, the game's own hints outside the pause screen are not drawn.
bool world_hints_hidden = false;
// The game's own hints not drawn, by their action (hide_game_hint).
std::vector<std::string> hidden_game_actions;
struct Hold {
  std::int64_t id = 0;
  ULONGLONG started = 0;
  bool done = false;
  // The press hint on the same control: it fires when the key comes up before the
  // hold ring is full; 0 without one.
  std::int64_t press_id = 0;
};
std::map<std::string, Hold> holds;  // control key -> the press being held

const void* reported_session = nullptr;

// The hints the HUD draws (ActionHint.shown): what the listener last drew,
// every hint whoever put it up, with its text in the player's language.
struct DrawnHint {
  std::string key;  // a resource hint's control, or the action a game hint triggers
  std::string map;
  std::string text;
  bool hold = false;
  bool enabled = true;
  float progress = 0.0f;
  // Who a prompt is about: the HUD names it in another element than the hint list, so it stays
  // empty for now.
  std::string target;

  bool operator==(const DrawnHint&) const = default;
};
// The hold ring of each action the game's own hints fill (the listener's SetProgress), by action.
std::map<std::string, float> ring_progress;
HoldProgress original_progress = nullptr;
bool progress_hooked = false;
// The drawn set, handed to the network thread; sent at most every kShownIntervalMs.
constexpr ULONGLONG kShownIntervalMs = 100;
std::vector<DrawnHint> drawn;  // engine thread
std::mutex drawn_mutex;
std::vector<DrawnHint> drawn_shared;
bool drawn_changed = false;
ULONGLONG drawn_told_at = 0;

const char* hint_text(const void* hint, std::size_t offset) {
  const char* text = nullptr;
  std::memcpy(&text, static_cast<const std::uint8_t*>(hint) + offset, sizeof(text));
  return text ? text : "";
}

// A control's hold hint has an S_ActionHint of its own beside its press hint.
constexpr char kHoldSuffix[] = "#hold";

std::string native_key(const std::string& key, bool hold) {
  return hold ? key + kHoldSuffix : key;
}

// The control key of one of the resource hints' S_ActionHints, or "" for a game hint.
std::string resource_key(const void* hint) {
  for (const auto& [name, ours] : native_hints) {
    if (ours == hint) {
      const std::size_t suffix = name.find(kHoldSuffix);
      return suffix == std::string::npos ? name : name.substr(0, suffix);
    }
  }
  return {};
}

void hand_over_drawn() {
  std::lock_guard<std::mutex> lock(drawn_mutex);
  drawn_shared = drawn;
  drawn_changed = true;
}

// What the HUD draws now, from the list the listener got.
void record_drawn(const std::vector<void*>& hints) {
  std::vector<DrawnHint> next;
  for (void* pointer : hints) {
    const auto* hint = static_cast<const std::uint8_t*>(pointer);
    if (!hint[kVisible]) {
      continue;
    }
    DrawnHint entry;
    const std::string action = hint_text(hint, kAction);
    const std::string ours = resource_key(hint);
    entry.key = ours.empty() ? action : ours;
    entry.map = hint_text(hint, kActionMap);
    entry.text = hint_text(hint, kText);
    // A localization key: flagged so, or an "@label" the HUD localizes itself (the game's own
    // prompts, "@ui_cook_item").
    const bool label = !entry.text.empty() && entry.text[0] == '@';
    if ((hint[kLocalized] || label) && !entry.text.empty()) {
      const std::string localized = branding::localize(label ? entry.text.substr(1) : entry.text);
      if (!localized.empty()) {
        entry.text = localized;
      }
    }
    entry.hold = hint[kType] == kTypeHold;
    entry.enabled = hint[kEnabled] != 0;
    const auto ring = ring_progress.find(action);
    entry.progress = ring != ring_progress.end() ? ring->second : 0.0f;
    next.push_back(std::move(entry));
  }
  if (next != drawn) {
    drawn = std::move(next);
    hand_over_drawn();
  }
}

// The listener's SetProgress (its second virtual): the hold ring of the hint on an
// action, which the game fills for its own hold hints.
double __fastcall hooked_progress(void* listener, float progress, void* action, int hint_class) {
  const char* name = action ? *static_cast<const char**>(action) : nullptr;
  if (name) {
    float& known = ring_progress[name];
    const bool moved = progress == 0.0f || progress >= 1.0f || progress - known >= 0.05f ||
                       known - progress >= 0.05f;
    if (moved && known != progress) {
      known = progress;
      for (DrawnHint& entry : drawn) {
        if (entry.key == name) {
          entry.progress = progress;
        }
      }
      hand_over_drawn();
    }
  }
  return original_progress(listener, progress, action, hint_class);
}

void hook_progress(void* listener) {
  if (progress_hooked || !listener) {
    return;
  }
  progress_hooked = true;
  void* target = reinterpret_cast<void*>((*reinterpret_cast<HoldProgress**>(listener))[1]);
  if (MH_CreateHook(target, reinterpret_cast<void*>(&hooked_progress),
                    reinterpret_cast<void**>(&original_progress)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    original_progress = nullptr;
    if (log_line) {
      log_line("ACTION HINTS could not hook the hold rings %p", target);
    }
  }
}

std::uint8_t* native_hint(const Control& control, bool hold) {
  const std::string name = native_key(control.key, hold);
  auto found = native_hints.find(name);
  if (found != native_hints.end()) {
    return found->second;
  }
  // Never freed: the HUD may keep the pointers it was given until the next update.
  auto hint = static_cast<std::uint8_t*>(::operator new(kHintSize));
  std::memset(hint, 0, kHintSize);
  construct_hint(hint);
  const bool other = hold && control.hold_action;
  assign_string(hint + kAction, other ? control.hold_action : control.action);
  assign_string(hint + kActionMap, other ? control.hold_actionmap : control.actionmap);
  native_hints[name] = hint;
  return hint;
}

// The resource hints as S_ActionHint pointers, written for this update.
std::vector<void*> resource_hints() {
  const std::vector<Shown> current = current_hints();
  std::vector<void*> out;
  for (const Shown& entry : current) {
    const Control* control = control_for(entry.key);
    if (!control) {
      continue;
    }
    std::uint8_t* hint = native_hint(*control, entry.hold);
    assign_string(hint + kText, entry.text.c_str());
    hint[kLocalized] = entry.localized ? 1 : 0;
    hint[kVisible] = 1;
    hint[kType] = entry.hold ? kTypeHold : kTypePress;
    // No special class: 7 to 11 are the game's chat, follow and similar rows.
    hint[kClass] = 0;
    *reinterpret_cast<float*>(hint + kHoldDuration) = static_cast<float>(entry.hold_seconds);
    hint[kEnabled] = entry.enabled ? 1 : 0;
    out.push_back(hint);
  }
  return out;
}

// Whether the game's own hint on this action is on a key a script's enabled hint takes (issue):
// the game's Pickpocket, Loot or Talk is not drawn beside the script's hint on E, as the
// press is the script's.
bool game_action_taken(const std::vector<Shown>& scripts, const std::string& action) {
  for (const Shown& entry : scripts) {
    const Control* control = control_for(entry.key);
    if (!entry.enabled || !control) {
      continue;
    }
    const bool hold_action = control->hold_action && action == control->hold_action;
    if (hold_action || control_action(*control, action.c_str())) {
      return true;
    }
  }
  return false;
}

void show(void* self, const std::vector<void*>& game_hints, unsigned flags) {
  std::vector<Shown> scripts;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    scripts = shown;
  }
  const bool menu = menu_open && menu_open();
  std::vector<void*> merged;
  for (void* hint : game_hints) {
    const std::uint8_t place = static_cast<std::uint8_t*>(hint)[kClass];
    const bool menu_place = place >= kFirstMenuClass && place <= kLastMenuClass;
    if ((menu_hints_hidden && menu_place) || (world_hints_hidden && !menu_place)) {
      continue;
    }
    const std::string action = hint_text(hint, kAction);
    if (std::find(hidden_game_actions.begin(), hidden_game_actions.end(), action) !=
        hidden_game_actions.end()) {
      continue;
    }
    if (!menu && game_action_taken(scripts, action)) {
      continue;
    }
    merged.push_back(hint);
  }
  std::vector<void*> ours = menu ? std::vector<void*>() : resource_hints();
  merged.insert(merged.end(), ours.begin(), ours.end());
  HintList list{merged.data(), merged.data() + merged.size(), merged.data() + merged.size()};
  original_listener(self, &list, flags);
  record_drawn(merged);
}

void __fastcall hooked_listener(void* self, HintList* hints, unsigned flags) {
  hook_progress(self);
  last_listener = self;
  last_flags = flags;
  last_game_hints.assign(hints->begin, hints->end);
  show(self, last_game_hints, flags);
}

// Fills the hold ring of the control's hold hint; 0 empties it.
void set_ring(const std::string& key, float progress) {
  auto found = native_hints.find(native_key(key, true));
  if (found == native_hints.end() || !last_listener) {
    return;
  }
  auto progress_call =
      original_progress ? original_progress : (*reinterpret_cast<HoldProgress**>(last_listener))[1];
  progress_call(last_listener, progress, found->second + kAction, 0);
  for (DrawnHint& entry : drawn) {
    if (entry.key == key && entry.progress != progress) {
      entry.progress = progress;
      hand_over_drawn();
    }
  }
}

void fire(const Shown& entry) {
  if (entry.on_fire) {
    entry.on_fire();
    return;
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  if (fired.size() < 64) {
    fired.push_back(entry.id);
  }
}

}  // namespace

bool install(const Natives& natives, Logger logger) {
  log_line = logger;
  construct_hint = reinterpret_cast<HintConstructor>(natives.hint_constructor);
  assign_string = natives.string_assign;
  menu_open = natives.menu_open;
  if (!natives.hud_hints_listener || !construct_hint || !assign_string) {
    return false;
  }
  return MH_CreateHook(natives.hud_hints_listener, reinterpret_cast<void*>(hooked_listener),
                       reinterpret_cast<void**>(&original_listener)) == MH_OK &&
         MH_EnableHook(natives.hud_hints_listener) == MH_OK;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "actionHints") {
    return false;
  }
  std::vector<Shown> next;
  const sdk::Value* list = kcd2s::integration::script_field(message, "hints");
  if (list && list->tag() == sdk::Tag::Array) {
    for (const sdk::Value& value : list->as_array()) {
      Shown entry;
      const sdk::Value* id = kcd2s::integration::script_field(value, "id");
      const sdk::Value* key = kcd2s::integration::script_field(value, "key");
      const sdk::Value* text = kcd2s::integration::script_field(value, "text");
      const sdk::Value* mode = kcd2s::integration::script_field(value, "mode");
      const sdk::Value* hold = kcd2s::integration::script_field(value, "holdDuration");
      const sdk::Value* enabled = kcd2s::integration::script_field(value, "enabled");
      if (!id || id->tag() != sdk::Tag::Int || !key || key->tag() != sdk::Tag::Str || !text ||
          text->tag() != sdk::Tag::Str || !control_for(key->as_str())) {
        continue;
      }
      entry.id = id->as_int();
      entry.key = key->as_str();
      entry.text = text->as_str().substr(0, 256);
      entry.hold = mode && mode->tag() == sdk::Tag::Str && mode->as_str() == "hold";
      if (hold && hold->tag() == sdk::Tag::Double) {
        entry.hold_seconds = hold->as_double();
      } else if (hold && hold->tag() == sdk::Tag::Int) {
        entry.hold_seconds = static_cast<double>(hold->as_int());
      }
      entry.enabled = !enabled || enabled->tag() != sdk::Tag::Bool || enabled->as_bool();
      next.push_back(std::move(entry));
    }
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  shown = std::move(next);
  shown_changed = true;
  return true;
}

bool on_player_action(const char* action, unsigned mode) {
  if (!action) {
    return false;
  }
  bool taken = false;
  const std::vector<Shown> current = current_hints();
  // The controls this action is on, each judged once: the first event of a press or release.
  std::unordered_map<std::string, bool> first;
  for (const Shown& entry : current) {
    const Control* control = control_for(entry.key);
    if (control && control_action(*control, action) && first.count(entry.key) == 0) {
      first[entry.key] = first_of_press(entry.key, mode);
    }
  }
  for (const Shown& entry : current) {
    const Control* control = control_for(entry.key);
    if (!control || !control_action(*control, action)) {
      continue;
    }
    // One more action of a press already taken: swallowed, nothing fires again.
    if (!first[entry.key]) {
      taken = taken || entry.enabled;
      if (log_line && mode == 1) {
        log_line("ACTION HINT %s: %s is the same press", entry.key.c_str(), action);
      }
      continue;
    }
    // An enabled hint's press is the hint's: the game's own action on that key does not run.
    taken = taken || entry.enabled;
    // A press hint beside a hold hint on the same control waits for the key to come up.
    const Shown* hold_beside = nullptr;
    for (const Shown& other : current) {
      if (other.key == entry.key && other.hold && other.enabled) {
        hold_beside = &other;
      }
    }
    if (mode == 1) {
      if (!entry.hold) {
        if (entry.enabled && !hold_beside) {
          fire(entry);
        }
      } else {
        Hold& hold = holds[entry.key];
        hold = Hold{entry.id, GetTickCount64(), false, 0};
        for (const Shown& other : current) {
          if (other.key == entry.key && !other.hold && other.enabled) {
            hold.press_id = other.id;
          }
        }
      }
    } else if (mode == 2 && entry.hold) {
      const auto found = holds.find(entry.key);
      // Let go before the ring was full: the press hint's press.
      if (found != holds.end() && !found->second.done && found->second.press_id != 0) {
        for (const Shown& other : current) {
          if (other.id == found->second.press_id && other.enabled) {
            fire(other);
          }
        }
      }
      if (found != holds.end()) {
        holds.erase(found);
      }
      // Only a hold hint has a ring; a progress call on a press hint draws the hold marker.
      set_ring(entry.key, 0);
    }
  }
  return taken;
}

// The script action key read from the keyboard (its game action is not active out in the world).
void poll_script_key(bool keys_open) {
  const Control* control = control_for("script_action");
  HKL layout = GetKeyboardLayout(0);
  const UINT key = MapVirtualKeyExW(control->scan, MAPVK_VSC_TO_VK, layout);
  const bool down = keys_open && key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000);
  if (down != script_key_down) {
    script_key_down = down;
    on_player_action(control->action, down ? 1 : 2);
  }
}

void frame(bool in_world, bool keys_open) {
  if (!in_world) {
    holds.clear();
    presses.clear();
    script_key_down = false;
    return;
  }
  poll_script_key(keys_open);
  // A long hint text is shown whole, not cut before its key.
  tick_hint_width(log_line);
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    changed = shown_changed;
    shown_changed = false;
  }
  changed = changed || engine_changed;
  engine_changed = false;
  const std::vector<Shown> current = current_hints();
  const ULONGLONG now = GetTickCount64();
  for (auto& [key, hold] : holds) {
    if (hold.done) {
      continue;
    }
    for (const Shown& entry : current) {
      if (entry.id != hold.id || !entry.enabled) {
        continue;
      }
      // The game's own ring fills while the key is held, as for its hold prompts.
      const double held_ms = static_cast<double>(now - hold.started);
      const double fill = held_ms / (entry.hold_seconds * 1000);
      if (fill >= 1) {
        hold.done = true;
        set_ring(key, 0);
        fire(entry);
      } else {
        set_ring(key, static_cast<float>(fill));
      }
    }
  }
  // The HUD keeps the last list it was given; redraw it with the new resource hints.
  if (changed && last_listener && original_listener) {
    show(last_listener, last_game_hints, last_flags);
  }
}

void set_engine_hint(const EngineHint& hint) {
  Shown entry;
  entry.id = hint.id;
  entry.key = hint.key;
  entry.text = hint.text;
  entry.hold = hint.hold;
  entry.hold_seconds = hint.hold_seconds;
  entry.enabled = hint.enabled;
  entry.localized = hint.localized;
  entry.on_fire = hint.on_fire;
  for (Shown& existing : engine_shown) {
    if (existing.id == hint.id) {
      const bool same = existing.key == entry.key && existing.text == entry.text &&
                        existing.enabled == entry.enabled;
      existing = std::move(entry);
      engine_changed = engine_changed || !same;
      return;
    }
  }
  engine_shown.push_back(std::move(entry));
  engine_changed = true;
}

void clear_engine_hint(std::int64_t id) {
  for (auto it = engine_shown.begin(); it != engine_shown.end(); ++it) {
    if (it->id == id) {
      engine_shown.erase(it);
      holds.clear();
      engine_changed = true;
      return;
    }
  }
}

void hide_menu_hints(bool hidden) {
  if (menu_hints_hidden == hidden) {
    return;
  }
  menu_hints_hidden = hidden;
  if (last_listener && original_listener) {
    show(last_listener, last_game_hints, last_flags);
  }
}

void hide_world_hints(bool hidden) {
  if (world_hints_hidden == hidden) {
    return;
  }
  world_hints_hidden = hidden;
  if (last_listener && original_listener) {
    show(last_listener, last_game_hints, last_flags);
  }
}

void hide_game_hint(const char* action, bool hidden) {
  const auto found = std::find(hidden_game_actions.begin(), hidden_game_actions.end(), action);
  const bool is_hidden = found != hidden_game_actions.end();
  if (is_hidden == hidden) {
    return;
  }
  if (hidden) {
    hidden_game_actions.emplace_back(action);
  } else {
    hidden_game_actions.erase(found);
  }
  if (last_listener && original_listener) {
    show(last_listener, last_game_hints, last_flags);
  }
}

void pump(const void* session, const RuntimeSender& send) {
  std::vector<std::int64_t> taken;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (session != reported_session) {
      reported_session = session;
      shown.clear();
      shown_changed = true;
      fired.clear();
      return;
    }
    taken.swap(fired);
  }
  for (std::int64_t id : taken) {
    sdk::Value::Map message{{"op", sdk::Value(std::string("actionHintFired"))},
                            {"id", sdk::Value(id)}};
    send(sdk::Value(std::move(message)));
  }
  // What the HUD draws, when it changed.
  std::vector<DrawnHint> now;
  {
    std::lock_guard<std::mutex> lock(drawn_mutex);
    const ULONGLONG tick = GetTickCount64();
    if (!drawn_changed || tick - drawn_told_at < kShownIntervalMs) {
      return;
    }
    drawn_changed = false;
    drawn_told_at = tick;
    now = drawn_shared;
  }
  sdk::Value::Array hints;
  for (const DrawnHint& entry : now) {
    sdk::Value::Map hint{{"key", sdk::Value(entry.key)},
                         {"map", sdk::Value(entry.map)},
                         {"text", sdk::Value(entry.text)},
                         {"mode", sdk::Value(std::string(entry.hold ? "hold" : "press"))},
                         {"enabled", sdk::Value(entry.enabled)},
                         {"progress", sdk::Value(double(entry.progress))},
                         {"target", sdk::Value(entry.target)}};
    hints.push_back(sdk::Value(std::move(hint)));
  }
  sdk::Value::Map message{{"op", sdk::Value(std::string("actionHintsShown"))},
                          {"hints", sdk::Value(std::move(hints))}};
  send(sdk::Value(std::move(message)));
}

}  // namespace kcds::runtime::action_hints
