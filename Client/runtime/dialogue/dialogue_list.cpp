#include "dialogue_list.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "../native_ui/native_ui.hpp"

namespace kcds::runtime::dialogue {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// The HUD element and its one instance (Libs/UI/UIElements/HUD.xml).
constexpr const char* kHud = "hud";
constexpr int kHudInstance = 0;

constexpr unsigned kPressed = 1;
constexpr unsigned kReleased = 2;

// The player's actions that drive the list, and those that still reach the game while it shows:
// looking around and the menus.
constexpr const char* kUpAction = "moveforward";
constexpr const char* kDownAction = "moveback";
constexpr const char* kPickAction = "use";
constexpr const char* kKeptPrefixes[] = {"rotate", "menu", "pause"};

struct Shown {
  integration::DialoguePage page;
  int active = 0;          // the highlighted row
  bool answered = false;   // a pick of this page went to the server
  bool drawn = false;      // the page is on the HUD
  bool highlight = false;  // the highlight changed since it was drawn
};

std::optional<Shown> shown;
// The page that was on the HUD last, so a close takes it off once.
bool on_screen = false;
std::vector<integration::DialogueChoice> choices;
std::vector<integration::DialogueCancel> cancels;
// The keys that were down last frame (edges only).
std::array<bool, 256> keys_down{};

// Only a fresh press of the use key picks a row (the E that opened a list at
// an NPC picked its first row 230 ms after it showed). The use action held now; whether it may
// pick (not while the key held when the list showed is not yet let go); when the list showed, and
// how long after that a use press picks nothing, far less than a player takes to read the rows.
bool use_held = false;
bool use_armed = true;
DWORD list_shown_at = 0;
constexpr DWORD kSettleMs = 350;

sdk::Value hud_call(const char* function, sdk::Value::Array args) {
  sdk::Value::Map body{{"op", sdk::Value(std::string("elementCall"))},
                       {"name", sdk::Value(std::string(kHud))},
                       {"instance", sdk::Value(static_cast<std::int64_t>(kHudInstance))},
                       {"fn", sdk::Value(std::string(function))},
                       {"args", sdk::Value(std::move(args))}};
  return native_ui::request(sdk::Value(std::move(body)));
}

bool hud_set_rows(const integration::DialoguePage& page) {
  // Per row, as C_UIHudInteractiveDialog's ShowSequences writes it: text, used,
  // skill check name, skill check difficulty, value.
  sdk::Value::Array values;
  for (const integration::DialogueOption& option : page.options) {
    values.push_back(sdk::Value(option.text));
    values.push_back(sdk::Value(false));
    values.push_back(sdk::Value(std::string()));
    values.push_back(sdk::Value(static_cast<std::int64_t>(0)));
    values.push_back(sdk::Value(0.0));
  }
  sdk::Value::Map body{{"op", sdk::Value(std::string("elementSetArray"))},
                       {"name", sdk::Value(std::string(kHud))},
                       {"instance", sdk::Value(static_cast<std::int64_t>(kHudInstance))},
                       {"array", sdk::Value(std::string("SequenceList"))},
                       {"values", sdk::Value(std::move(values))}};
  const sdk::Value set = native_ui::request(sdk::Value(std::move(body)));
  return set.tag() == sdk::Tag::Bool && set.as_bool();
}

// The HUD's DialogLeft / DialogRight clip holds the rows. The game shows it only while its own
// conversation runs and sets it back every frame, so a shown page shows its clip every frame.
void set_dialog_clip(const char* clip, bool visible) {
  sdk::Value::Map body{{"op", sdk::Value(std::string("elementSetClip"))},
                       {"name", sdk::Value(std::string(kHud))},
                       {"instance", sdk::Value(static_cast<std::int64_t>(kHudInstance))},
                       {"clip", sdk::Value(std::string(clip))},
                       {"visible", sdk::Value(visible)}};
  native_ui::request(sdk::Value(std::move(body)));
}

void show_dialog_clip(bool on_right) {
  set_dialog_clip(on_right ? "DialogRight" : "DialogLeft", true);
}

void hud_clear() {
  hud_call("ClearDialogSequences", {});
  hud_call("ClearSubtitles", {});
  set_dialog_clip("DialogLeft", false);
  set_dialog_clip("DialogRight", false);
}

// Draws the page; false when the HUD could not take it.
bool draw(const Shown& state) {
  const integration::DialoguePage& page = state.page;
  if (!native_ui::element_instance(kHud, kHudInstance)) {
    return false;
  }
  hud_call("ClearDialogSequences", {});
  if (!hud_set_rows(page)) {
    return false;
  }
  // A function without a result answers undefined (null here), so only the rows tell failure.
  hud_call("SetDialogSequences", {sdk::Value(page.on_right)});
  for (std::size_t index = 0; index < page.options.size(); ++index) {
    if (!page.options[index].enabled) {
      hud_call("SetEnableSequenceChanged",
               {sdk::Value(static_cast<std::int64_t>(index)), sdk::Value(false)});
    }
  }
  hud_call("SetActiveSequenceChanged", {sdk::Value(static_cast<std::int64_t>(state.active)),
                                        sdk::Value(static_cast<std::int64_t>(0))});
  if (page.line.empty()) {
    hud_call("ClearSubtitles", {});
  } else {
    hud_call("SetSubtitles", {sdk::Value(page.line), sdk::Value(page.speaker)});
  }
  return true;
}

// The first enabled row from `from` in `step` direction, wrapping; `from` when there is none.
int next_enabled(const integration::DialoguePage& page, int from, int step) {
  const int count = static_cast<int>(page.options.size());
  for (int moved = 1; moved <= count; ++moved) {
    const int index = ((from + step * moved) % count + count) % count;
    if (page.options[static_cast<std::size_t>(index)].enabled) {
      return index;
    }
  }
  return from;
}

void move(int step) {
  if (!shown) {
    return;
  }
  const int active = next_enabled(shown->page, shown->active, step);
  if (active != shown->active) {
    shown->active = active;
    shown->highlight = true;
  }
}

void pick(int index) {
  if (!shown || shown->answered || index < 0 ||
      index >= static_cast<int>(shown->page.options.size())) {
    return;
  }
  if (!shown->page.options[static_cast<std::size_t>(index)].enabled) {
    return;
  }
  if (index != shown->active) {
    shown->active = index;
    shown->highlight = true;
  }
  integration::DialogueChoice choice;
  choice.session = shown->page.session;
  choice.generation = shown->page.generation;
  choice.index = static_cast<std::uint8_t>(index);
  choices.push_back(choice);
  shown->answered = true;
}

// Ends the conversation on this side and tells the server why.
void end(integration::DialogueCloseReason reason) {
  if (!shown) {
    return;
  }
  integration::DialogueCancel cancel;
  cancel.session = shown->page.session;
  cancel.reason = reason;
  cancels.push_back(cancel);
  shown.reset();
}

bool starts_with(const char* text, const char* prefix) {
  return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

// A key's press since the last frame.
bool pressed(int key) {
  const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
  const bool edge = down && !keys_down[static_cast<std::size_t>(key)];
  keys_down[static_cast<std::size_t>(key)] = down;
  return edge;
}

// The arrow keys are not read here: the game binds them with W and S to moveforward and moveback
// (Libs/Config/keybindSuperactions.xml, superactions up and down), which on_action takes, and a
// press read twice moved two rows (on three rows Down went up, on two rows the
// arrows did nothing).
void poll_keys() {
  if (pressed(VK_RETURN) && shown) {
    pick(shown->active);
  }
  for (int row = 0; row < static_cast<int>(integration::kMaxDialogueOptions); ++row) {
    if (pressed('1' + row)) {
      pick(row);
    }
  }
  if (pressed(VK_BACK)) {
    end(integration::DialogueCloseReason::Cancelled);
  }
}

// Forgets the keys held, so a key held while the list came up does not count as a press.
void sample_keys() {
  const int keys[] = {VK_RETURN, VK_BACK};
  for (int key : keys) {
    keys_down[static_cast<std::size_t>(key)] = (GetAsyncKeyState(key) & 0x8000) != 0;
  }
  for (int row = 0; row < static_cast<int>(integration::kMaxDialogueOptions); ++row) {
    keys_down[static_cast<std::size_t>('1' + row)] = (GetAsyncKeyState('1' + row) & 0x8000) != 0;
  }
}

}  // namespace

void apply(const integration::DialogueOrder& order) {
  if (const auto* close = std::get_if<integration::DialogueClose>(&order)) {
    if (shown && shown->page.session == close->session) {
      shown.reset();
    }
    return;
  }
  const auto& page = std::get<integration::DialoguePage>(order);
  if (!shown) {
    sample_keys();
  }
  // The press that asked for this page (the key still held, or a press right after it showed:
  // the first page's E at an NPC, a pick's E that the next page follows) picks nothing on it.
  use_armed = !use_held;
  list_shown_at = GetTickCount();
  Shown next;
  next.page = page;
  next.active = page.options[0].enabled ? 0 : next_enabled(page, 0, 1);
  shown = std::move(next);
}

bool on_action(const char* action, unsigned mode) {
  if (action && std::strcmp(action, kPickAction) == 0) {
    // The use key is followed with or without a list: the list arms it on a release.
    if (mode == kPressed) {
      use_held = true;
    } else if (mode == kReleased) {
      use_held = false;
      use_armed = true;
    }
  }
  if (!shown || !action) {
    return false;
  }
  if (mode == kReleased) {
    return false;
  }
  for (const char* prefix : kKeptPrefixes) {
    if (starts_with(action, prefix)) {
      return false;
    }
  }
  if (mode == kPressed) {
    if (std::strcmp(action, kUpAction) == 0) {
      move(-1);
    } else if (std::strcmp(action, kDownAction) == 0) {
      move(1);
    } else if (std::strcmp(action, kPickAction) == 0 && use_armed &&
               GetTickCount() - list_shown_at >= kSettleMs) {
      pick(shown->active);
    }
  }
  return true;
}

bool showing() {
  return shown.has_value();
}

void frame(bool in_world, bool game_in_dialogue, bool keys) {
  if (!in_world) {
    reset();
    return;
  }
  if (shown && game_in_dialogue) {
    // The game's own conversation took the list.
    end(integration::DialogueCloseReason::Interrupted);
    on_screen = false;
    return;
  }
  if (shown && keys) {
    poll_keys();
  }
  if (!shown) {
    if (on_screen) {
      hud_clear();
      on_screen = false;
    }
    return;
  }
  if (!shown->drawn) {
    if (!draw(*shown)) {
      // No HUD to show the list on: the conversation cannot go on.
      end(integration::DialogueCloseReason::Interrupted);
      hud_clear();
      on_screen = false;
      return;
    }
    shown->drawn = true;
    shown->highlight = false;
    on_screen = true;
  }
  show_dialog_clip(shown->page.on_right);
  if (shown->highlight) {
    hud_call("SetActiveSequenceChanged", {sdk::Value(static_cast<std::int64_t>(shown->active)),
                                          sdk::Value(static_cast<std::int64_t>(0))});
    shown->highlight = false;
  }
}

std::vector<integration::DialogueChoice> take_choices() {
  return std::exchange(choices, {});
}

std::vector<integration::DialogueCancel> take_cancels() {
  return std::exchange(cancels, {});
}

void reset() {
  if (on_screen) {
    hud_clear();
    on_screen = false;
  }
  shown.reset();
  choices.clear();
  cancels.clear();
}

}  // namespace kcds::runtime::dialogue
