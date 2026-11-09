#include "knockout.hpp"
#include "fall_knockout.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../hud/resource_hud.hpp"
#include "MinHook.h"
#include "faint.hpp"

namespace kcds::runtime::knockout {
namespace {

using GetBuffManager = void*(__fastcall*)();
using SetSoulState = void(__fastcall*)(void* soul, unsigned state, float value);
// gEnv->pRenderer's SetPostEffectParamVec4 (vtable slot 178), as the flow node Image:ScreenFader
// sets the screen fader's color.
struct Color {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;
};
using SetPostEffectColor = void(__fastcall*)(void* renderer, const char* name, const Color* value,
                                             bool force);
constexpr std::size_t kSetPostEffectColor = 178;
using GetGame = void*(__fastcall*)();
// The UI element the game shows 1.3 s after the player loses consciousness: the black screen with
// the seal. Shown with the UI's vtable slot 19 (element, owner) and hidden with
// slot 20 when the player wakes. The UI is game+24, its slot 35, then that one's slot 7.
using ShowUiElement = std::uintptr_t(__fastcall*)(void* ui, std::uint8_t element, void* owner);
constexpr std::uint8_t kKnockoutScreen = 0xAF;
constexpr std::size_t kGameUiRoot = 24;
constexpr std::size_t kUiRootSystem = 35;
constexpr std::size_t kUiSystemElements = 7;
constexpr std::size_t kShowUiElement = 19;

// A buff id as the game keeps it (the standard GUID layout).
struct BuffId {
  std::uint32_t data1 = 0;
  std::uint16_t data2 = 0;
  std::uint16_t data3 = 0;
  std::uint8_t data4[8] = {};
};

constexpr unsigned hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return unsigned(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return unsigned(c - 'a' + 10);
  }
  return unsigned(c - 'A' + 10);
}

// Reads `digits` hex digits of the text at `at`, skipping the dashes.
constexpr std::uint32_t hex_value(const char* text, std::size_t& at, unsigned digits) {
  std::uint32_t value = 0;
  for (unsigned read = 0; read < digits; ++at) {
    if (text[at] == '-') {
      continue;
    }
    value = value * 16 + hex_digit(text[at]);
    ++read;
  }
  return value;
}

// A buff id from its text in rpg/buff.xml ("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"), so the
// constants below are the table's own text (a hand-split byte array once named a buff that does
// not exist, and the game refused it).
constexpr BuffId buff_id(const char* text) {
  BuffId id;
  std::size_t at = 0;
  id.data1 = hex_value(text, at, 8);
  id.data2 = static_cast<std::uint16_t>(hex_value(text, at, 4));
  id.data3 = static_cast<std::uint16_t>(hex_value(text, at, 4));
  for (std::uint8_t& byte : id.data4) {
    byte = static_cast<std::uint8_t>(hex_value(text, at, 2));
  }
  return id;
}

// The buff's parameters; the script binding passes them all zero.
struct BuffParams {
  std::uint8_t bytes[16] = {};
};

// C_BuffManager slot 0: adds a buff to a soul and returns its instance (nullptr when refused).
using AddBuff = void*(__fastcall*)(void* manager, void* soul, const BuffId* id, void* source,
                                   const BuffParams* params);
constexpr std::size_t kAddBuff = 0;

// rpg/buff.xml "immortality_nonpersistent" (imm=1; no UI name, no icon, UI visibility None, so the
// player never sees it).
constexpr BuffId kImmortality = buff_id("730503bf-735a-4f47-baae-c2d84ee77524");
// rpg/buff.xml "remove_unconsciousness" (hidden the same way): the game's own end of a knock-out.
// It belongs to the class "Unconsciousness" and is exclusive within it, so it replaces the buff
// that keeps the soul unconscious and then expires at once.
constexpr BuffId kRemoveUnconsciousness = buff_id("bd22f98a-e61f-4d83-b39c-79d1d85b6b91");
// rpg/buff.xml "unconscious_permanent" (class "Unconsciousness", no end of its own): what keeps a
// knocked-out player's replica down in every other game.
constexpr BuffId kUnconsciousPermanent = buff_id("c75aa0db-65ca-44d7-9001-e4b6d38c6875");

// The soul's buff instances: a vector of pointers (the buff manager appends to it).
constexpr std::size_t kSoulBuffsBegin = 1352;
constexpr std::size_t kSoulBuffsEnd = 1360;
// The soul's health (the same cache the combat code reads).
constexpr std::size_t kSoulHealth = 1800;
// C_Soul vtable slot 7, IsUnconscious (health above 0 and the consciousness
// state 55 at 0), the game's own UnconsciousCheck. A lethal hit on an immortal soul leaves it at
// ImmortalHealthMin (1), but it regenerates while it lies: at 1.19 a health threshold no longer
// saw the knock-out, so the server refused the revive and the game's knock-out screen showed
constexpr std::size_t kSoulIsUnconscious = 7;
// The immortality is checked about once a second (a pointer comparison) and added again when the
// game dropped it (a loaded save drops non-persistent buffs).
constexpr int kImmortalityCheckFrames = 60;
// The soul state "health" (state 0; the setter clamps the value to the soul's maximum).
constexpr unsigned kHealthState = 0;
constexpr float kFullHealth = 1000.0f;
// The soul state "consciousness" (state 55, IsUnconscious's): held at 0 while the player is kept
// down, so the moment the game's own knock-out wears off it is still unconscious (it stood up
// for about two seconds before the permanent unconsciousness took hold, and every other player saw
// it get up), and full again at a revive.
constexpr unsigned kConsciousnessState = 55;
constexpr float kFullConsciousness = 1000.0f;
// The knock-out darkens the view to black with the game's UI faders, which its console variable
// wh_ui_FaderSuspend holds back (the view stays the game's red low-health view of the world
// from the ground). IConsole::GetCVar is console slot 0xB8 / 8, ICVar::Set(text) is cvar slot
// 0x48 / 8 (the build info setting uses the same).
constexpr char kFaderSuspend[] = "wh_ui_FaderSuspend";
constexpr std::size_t kConsoleGetCVar = 0xB8 / sizeof(void*);
constexpr std::size_t kCVarSetText = 0x48 / sizeof(void*);
// The view is then darkened to the vision with the renderer's own screen fader: black, with the
// opacity of what the player does not see. Clear when nothing darkens it.
constexpr char kScreenFaderColor[] = "ScreenFader_Color";

Logger log_line = nullptr;
Natives natives;
void* current_soul = nullptr;
void* immortality = nullptr;
int frames_to_immortality_check = 0;
bool local_in_air = false;
void* fall_soul = nullptr;
FallKnockout fall_check;
// The local player lies knocked out (unconscious, and not in the faint).
bool down = false;
// The local player lies in the game's exhaustion faint, which the game ends itself.
bool in_faint = false;
// The local player stays knocked out until revive(): the game's own unconsciousness wears off by
// itself (up after some minutes, unrevived), so the permanent one is added at the knock-out
// and given again, at most once a second, if the game ends it anyway.
bool kept_down = false;
std::chrono::steady_clock::time_point kept_down_at;
// When the local player was last revived (GetTickCount64), 0 for never.
std::uint64_t revived_ms = 0;
// The vision this module applies; kNoVision while the player is not knocked out.
constexpr int kNoVision = -1;
int applied_vision = kNoVision;
// The resource's knock-out vision in percent.
unsigned vision = 0;
ShowUiElement original_show_ui_element = nullptr;
bool ui_hooked = false;
// Replicas held down: player -> its soul and the unconsciousness on it.
struct ReplicaDown {
  void* soul = nullptr;
  void* buff = nullptr;
  std::chrono::steady_clock::time_point added;
  // Held down for its owner's faint: its own game lays it down.
  bool fainted = false;
};
// A replica's lost unconsciousness is given again at most this often (one game dropped the
// added buff at once, it was added again every frame and the body twitched on the ground).
constexpr auto kReplicaDownRetry = std::chrono::seconds(1);
std::unordered_map<std::uint32_t, ReplicaDown> replicas_down;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

float health_of(void* soul) {
  return *reinterpret_cast<const float*>(static_cast<std::uint8_t*>(soul) + kSoulHealth);
}

void* add_buff(void* soul, const BuffId& id) {
  void* manager = reinterpret_cast<GetBuffManager>(natives.buff_manager)();
  if (!manager) {
    return nullptr;
  }
  const BuffParams params;
  return slot<AddBuff>(manager, kAddBuff)(manager, soul, &id, nullptr, &params);
}

// Whether the soul still holds this buff instance (only the pointers are compared).
bool has_buff(void* soul, void* instance) {
  if (!instance) {
    return false;
  }
  auto bytes = static_cast<std::uint8_t*>(soul);
  auto begin = *reinterpret_cast<void***>(bytes + kSoulBuffsBegin);
  auto end = *reinterpret_cast<void***>(bytes + kSoulBuffsEnd);
  for (auto it = begin; it && it != end; ++it) {
    if (*it == instance) {
      return true;
    }
  }
  return false;
}

// Whether the soul holds any instance of this buff (compared by its definition's GUID: the game may
// keep its own instance of a buff and drop the one added again).
bool holds_buff(void* soul, const BuffId& id) {
  constexpr std::size_t kInstanceDefinition = 16;
  auto bytes = static_cast<std::uint8_t*>(soul);
  auto begin = *reinterpret_cast<std::uint8_t***>(bytes + kSoulBuffsBegin);
  auto end = *reinterpret_cast<std::uint8_t***>(bytes + kSoulBuffsEnd);
  for (auto it = begin; it && it != end; ++it) {
    const void* definition = *it ? *reinterpret_cast<void**>(*it + kInstanceDefinition) : nullptr;
    if (definition && std::memcmp(definition, &id, sizeof id) == 0) {
      return true;
    }
  }
  return false;
}

std::size_t buff_count(void* soul) {
  auto bytes = static_cast<std::uint8_t*>(soul);
  auto begin = *reinterpret_cast<void***>(bytes + kSoulBuffsBegin);
  auto end = *reinterpret_cast<void***>(bytes + kSoulBuffsEnd);
  return begin ? std::size_t(end - begin) : 0;
}

void keep_immortal(void* soul) {
  if (frames_to_immortality_check > 0) {
    --frames_to_immortality_check;
    return;
  }
  frames_to_immortality_check = kImmortalityCheckFrames;
  if (has_buff(soul, immortality)) {
    return;
  }
  immortality = add_buff(soul, kImmortality);
  if (log_line) {
    log_line("KNOCKOUT immortality added=%u", unsigned(immortality != nullptr));
  }
}

// The knock-out's own screen is never shown: it has the game's loading indicator in its corner
//. The world is dimmed to the vision instead, black at 0.
std::uintptr_t __fastcall hooked_show_ui_element(void* ui, std::uint8_t element, void* owner) {
  if (element == kKnockoutScreen && (down || kept_down)) {
    return 0;
  }
  return original_show_ui_element(ui, element, owner);
}

// The UI exists once the game runs a world, so its element show is hooked on the first tick with
// a soul (engine thread), long before a knock-out shows the screen.
void hook_ui() {
  if (ui_hooked) {
    return;
  }
  ui_hooked = true;
  auto game = static_cast<std::uint8_t*>(reinterpret_cast<GetGame>(natives.game)());
  void* root = game ? *reinterpret_cast<void**>(game + kGameUiRoot) : nullptr;
  void* system = root ? slot<void*(__fastcall*)(void*)>(root, kUiRootSystem)(root) : nullptr;
  void* elements =
      system ? slot<void*(__fastcall*)(void*)>(system, kUiSystemElements)(system) : nullptr;
  void* show = elements ? (*reinterpret_cast<void***>(elements))[kShowUiElement] : nullptr;
  const bool hooked = show &&
                      MH_CreateHook(show, reinterpret_cast<void*>(hooked_show_ui_element),
                                    reinterpret_cast<void**>(&original_show_ui_element)) == MH_OK &&
                      MH_EnableHook(show) == MH_OK;
  if (log_line) {
    log_line("KNOCKOUT screen hook=%u", unsigned(hooked));
  }
}

void set_screen_darkness(float opacity) {
  void* renderer = natives.renderer ? *natives.renderer : nullptr;
  if (!renderer) {
    return;
  }
  Color color;
  color.a = opacity;
  slot<SetPostEffectColor>(renderer, kSetPostEffectColor)(renderer, kScreenFaderColor, &color,
                                                          true);
}

void suspend_faders(bool suspended) {
  void* console = natives.console_slot ? *natives.console_slot : nullptr;
  using GetCVar = void*(__fastcall*)(void* console, const char* name);
  void* cvar = console ? slot<GetCVar>(console, kConsoleGetCVar)(console, kFaderSuspend) : nullptr;
  if (!cvar) {
    return;
  }
  using SetText = void(__fastcall*)(void* cvar, const char* value);
  slot<SetText>(cvar, kCVarSetText)(cvar, suspended ? "1" : "0");
}

// While the player lies knocked out it sees the world dimmed to the vision (all black at 0); once
// it gets up the game's own behavior is back. Only a change is applied. Kept down counts as down:
// after about 7 minutes the game ends its own unconsciousness, and for the frames until it is
// given again the view cleared and showed the world.
void update_vision() {
  const int wanted = (down || kept_down) ? int(vision) : kNoVision;
  if (wanted == applied_vision) {
    return;
  }
  if (wanted != kNoVision) {
    suspend_faders(true);
    set_screen_darkness(1.0f - float(wanted) / 100.0f);
  } else {
    set_screen_darkness(0.0f);
    suspend_faders(false);
  }
  // The health and stamina bar goes while down: on the dimmed view it reads as a loading bar.
  kcds::runtime::hud::set_knocked_out(wanted != kNoVision);
  applied_vision = wanted;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (log_line && !natives.unconscious_vtable) {
    log_line("KNOCKOUT unconscious buff class missing: a faint counts as a knock-out");
  }
  return natives.buff_manager && natives.set_state && natives.renderer && natives.game &&
         natives.console_slot;
}

bool tick(void* soul, unsigned vision_percent) {
  vision = vision_percent;
  if (soul != current_soul) {
    current_soul = soul;
    immortality = nullptr;
    frames_to_immortality_check = 0;
    down = false;
    in_faint = false;
    kept_down = false;
  }
  if (!soul) {
    in_faint = false;
    update_vision();
    return false;
  }
  hook_ui();
  keep_immortal(soul);
  const float health = health_of(soul);
  const bool was_down = down;
  const bool was_fainted = in_faint;
  const bool unconscious = slot<bool(__fastcall*)(void*)>(soul, kSoulIsUnconscious)(soul);
  // A faint is the game's own, and it ends it. Kept down after a knock-out, the
  // player stays knocked out even if exhaustion makes it faint meanwhile.
  in_faint = unconscious && !kept_down && faint::holds_faint(soul, natives.unconscious_vtable);
  down = unconscious && !in_faint;
  if (in_faint != was_fainted && log_line) {
    log_line(in_faint ? "KNOCKOUT local player fainted from exhaustion health=%.2f"
                      : "KNOCKOUT local player out of the faint health=%.2f",
             health);
  }
  const auto now = std::chrono::steady_clock::now();
  if (kept_down && !down) {
    reinterpret_cast<SetSoulState>(natives.set_state)(soul, kConsciousnessState, 0.0f);
  }
  if (kept_down && !down && now - kept_down_at >= kReplicaDownRetry) {
    // Added while the knock-out's own unconsciousness still ran, the permanent one expired at once
    //, so it is given when that one has ended.
    kept_down_at = now;
    add_buff(soul, kUnconsciousPermanent);
    if (log_line) {
      log_line("KNOCKOUT local player kept down: the game ended the unconsciousness unrevived");
    }
  }
  update_vision();
  if (was_down && !down && log_line) {
    log_line("KNOCKOUT local player's unconsciousness ended health=%.2f", health);
  }
  if (!down || was_down || kept_down) {
    return false;
  }
  kept_down = true;
  reinterpret_cast<SetSoulState>(natives.set_state)(soul, kConsciousnessState, 0.0f);
  if (log_line) {
    log_line("KNOCKOUT local player knocked out health=%.2f", health);
  }
  return true;
}

bool revive() {
  if (!current_soul || !natives.set_state || !natives.buff_manager) {
    return false;
  }
  kept_down = false;
  revived_ms = GetTickCount64();
  // Full health and the end of the knock-out, both applied at once like the script calls
  // SetState('health') and AddBuff(remove_unconsciousness).
  reinterpret_cast<SetSoulState>(natives.set_state)(current_soul, kHealthState, kFullHealth);
  reinterpret_cast<SetSoulState>(natives.set_state)(current_soul, kConsciousnessState,
                                                    kFullConsciousness);
  const bool woke = add_buff(current_soul, kRemoveUnconsciousness) != nullptr;
  if (log_line) {
    log_line("KNOCKOUT revive woke=%u", unsigned(woke));
  }
  return woke;
}

bool knock_out(float health) {
  if (!current_soul || !natives.set_state || !natives.buff_manager) {
    return false;
  }
  // The health it is left with, then the game's own permanent unconsciousness; the next tick
  // sees the player down and reports it as for a lethal hit.
  reinterpret_cast<SetSoulState>(natives.set_state)(current_soul, kHealthState, health);
  const bool fell = add_buff(current_soul, kUnconsciousPermanent) != nullptr;
  if (log_line) {
    log_line("KNOCKOUT knocked out by the server health=%.2f taken=%u", health, unsigned(fell));
  }
  return fell;
}

bool knocked_out() {
  return down;
}

std::uint64_t revived_at() {
  return revived_ms;
}

bool kept_down_now() {
  return kept_down;
}

bool fainted() {
  return in_faint;
}

void note_in_air(bool in_air) {
  local_in_air = in_air;
}

bool fall_took_last_health(void* soul, std::uint64_t now_ms) {
  if (!soul) {
    return false;
  }
  if (soul != fall_soul) {
    fall_soul = soul;
    fall_check = FallKnockout{};
  }
  // A knocked-out body is already down, and one kept down has fallen for the last time.
  const bool lethal = fall_check.update(now_ms, local_in_air, health_of(soul));
  return lethal && !down && !kept_down && !in_faint;
}

}  // namespace kcds::runtime::knockout

namespace kcds::runtime::knockout {

void keep_replica_down(std::uint32_t player, void* soul, bool owner_down, bool owner_fainted) {
  if (!natives.buff_manager || !soul) {
    return;
  }
  auto it = replicas_down.find(player);
  if (owner_down) {
    const auto now = std::chrono::steady_clock::now();
    const bool same_body = it != replicas_down.end() && it->second.soul == soul;
    if (same_body && owner_fainted && !it->second.fainted) {
      it->second.fainted = true;
      if (log_line) {
        log_line("KNOCKOUT replica of player=%u down for its owner's faint", player);
      }
    }
    if (same_body && (has_buff(soul, it->second.buff) || holds_buff(soul, kUnconsciousPermanent) ||
                      now - it->second.added < kReplicaDownRetry)) {
      return;
    }
    const bool faint_down = owner_fainted || (same_body && it->second.fainted);
    void* buff = add_buff(soul, kUnconsciousPermanent);
    replicas_down[player] = ReplicaDown{soul, buff, now, faint_down};
    if (log_line) {
      log_line("KNOCKOUT replica of player=%u down (unconsciousness=%u, buffs=%zu, faint=%u)",
               player, unsigned(buff != nullptr), buff_count(soul), unsigned(faint_down));
    }
    return;
  }
  if (it == replicas_down.end()) {
    return;
  }
  if (it->second.soul == soul) {
    add_buff(soul, kRemoveUnconsciousness);
  }
  replicas_down.erase(it);
  if (log_line) {
    log_line("KNOCKOUT replica of player=%u up", player);
  }
}

bool replica_down(std::uint32_t player) {
  return replicas_down.count(player) != 0;
}

bool replica_fainted(std::uint32_t player) {
  const auto it = replicas_down.find(player);
  return it != replicas_down.end() && it->second.fainted;
}

void forget_replica(std::uint32_t player) {
  replicas_down.erase(player);
}

void keep_creature_immortal(void* soul) {
  if (!natives.buff_manager || !soul || holds_buff(soul, kImmortality)) {
    return;
  }
  const bool added = add_buff(soul, kImmortality) != nullptr;
  if (log_line) {
    log_line("KNOCKOUT creature soul=%p immortality added=%u", soul, unsigned(added));
  }
}

void keep_untouchable(void* soul) {
  // About twice a second: the buffs are searched and the health state set.
  constexpr int kUntouchableCheckFrames = 30;
  static int frames = 0;
  if (!soul || !natives.set_state || !natives.buff_manager || ++frames < kUntouchableCheckFrames) {
    return;
  }
  frames = 0;
  keep_creature_immortal(soul);
  reinterpret_cast<SetSoulState>(natives.set_state)(soul, kHealthState, kFullHealth);
}

bool creature_unconscious(void* soul) {
  return soul && slot<bool(__fastcall*)(void*)>(soul, kSoulIsUnconscious)(soul);
}

void hold_creature_down(void* soul) {
  if (!natives.buff_manager || !soul || holds_buff(soul, kUnconsciousPermanent)) {
    return;
  }
  const bool added = add_buff(soul, kUnconsciousPermanent) != nullptr;
  if (log_line) {
    log_line("KNOCKOUT creature soul=%p held down=%u", soul, unsigned(added));
  }
}

void wake_creature(void* soul) {
  if (!natives.buff_manager || !natives.set_state || !soul) {
    return;
  }
  reinterpret_cast<SetSoulState>(natives.set_state)(soul, kHealthState, kFullHealth);
  const bool woke = add_buff(soul, kRemoveUnconsciousness) != nullptr;
  if (log_line) {
    log_line("KNOCKOUT creature soul=%p woke=%u", soul, unsigned(woke));
  }
}

}  // namespace kcds::runtime::knockout
