#include "keybinds.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <map>
#include <mutex>
#include <vector>

#include "../hud/game_texts.hpp"
#include "keybind_screen.hpp"
#include "keybind_store.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::keybinds {
namespace {

namespace sdk = kcd2s::sdk;

constexpr std::size_t kMaxName = 64;
constexpr std::size_t kMaxLabel = 128;
constexpr std::size_t kMaxActions = 256;
constexpr std::size_t kMaxKeys = 4;
// How often the player's choices on the screen are read.
constexpr ULONGLONG kPollMs = 250;

struct Wanted {
  screen::Action action;
  std::string controller;  // default controller input, or ""
};

struct Row {
  std::string name;
  std::vector<std::string> keys;
  std::string controller;

  bool operator==(const Row& other) const {
    return name == other.name && keys == other.keys && controller == other.controller;
  }
};

std::mutex state_mutex;
std::wstring store;
Choices choices;
std::vector<screen::Group> groups;
std::vector<Wanted> actions;
bool shown_dirty = false;
std::map<std::string, std::vector<std::string>> pending_keys;
std::vector<Row> report;
unsigned report_revision = 0;

// Network thread only.
const void* reported_session = nullptr;
unsigned sent_revision = 0;

// Engine thread only.
ULONGLONG last_poll = 0;

// kcds_ names and the engine's input names: lower-case letters, digits and underscores.
bool plain(const std::string& text, bool may_be_empty) {
  if (text.empty()) {
    return may_be_empty;
  }
  if (text.size() > kMaxName) {
    return false;
  }
  for (char c : text) {
    const bool letter = c >= 'a' && c <= 'z';
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != '_') {
      return false;
    }
  }
  return true;
}

bool ours(const std::string& name) {
  return name.rfind("kcds_", 0) == 0 && plain(name, false);
}

std::string text_field(const sdk::Value& value, const char* name) {
  const sdk::Value* field = kcd2s::integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

const sdk::Value* list_field(const sdk::Value& value, const char* name) {
  const sdk::Value* field = kcd2s::integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Array ? field : nullptr;
}

std::vector<std::string> inputs_of(const sdk::Value& list) {
  std::vector<std::string> inputs;
  for (const sdk::Value& entry : list.as_array()) {
    if (entry.tag() == sdk::Tag::Str && plain(entry.as_str(), false) && inputs.size() < kMaxKeys) {
      inputs.push_back(entry.as_str());
    }
  }
  return inputs;
}

// The game client's own actions and their groups, which every resource set keeps.
std::vector<Wanted> builtin_actions;
std::vector<screen::Group> builtin_groups;

// Leaves only the game client's own actions on the screen (state_mutex held): a server whose
// resources tell no actions of their own still shows voice chat's keys.
void keep_only_builtins() {
  groups = builtin_groups;
  actions = builtin_actions;
  pending_keys.clear();
  shown_dirty = true;
}

void read_set(const sdk::Value& message) {
  std::vector<screen::Group> new_groups;
  if (const sdk::Value* list = list_field(message, "groups")) {
    for (const sdk::Value& entry : list->as_array()) {
      screen::Group group{text_field(entry, "name"), text_field(entry, "label")};
      if (ours(group.name) && group.label.size() <= kMaxLabel) {
        new_groups.push_back(std::move(group));
      }
    }
  }
  std::vector<Wanted> new_actions;
  if (const sdk::Value* list = list_field(message, "actions")) {
    for (const sdk::Value& entry : list->as_array()) {
      Wanted wanted;
      wanted.action.name = text_field(entry, "name");
      wanted.action.group = text_field(entry, "group");
      wanted.action.label = text_field(entry, "label");
      wanted.action.keyboard = text_field(entry, "key");
      wanted.controller = text_field(entry, "controller");
      const bool valid = ours(wanted.action.name) && plain(wanted.action.group, false) &&
                         plain(wanted.action.keyboard, true) && plain(wanted.controller, true) &&
                         wanted.action.label.size() <= kMaxLabel;
      if (valid && new_actions.size() < kMaxActions) {
        new_actions.push_back(std::move(wanted));
      }
    }
  }
  for (const screen::Group& group : new_groups) {
    hud::set_text(group.name, group.label);
  }
  for (const Wanted& wanted : new_actions) {
    hud::set_text(wanted.action.name, wanted.action.label);
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  for (const Wanted& builtin : builtin_actions) {
    new_actions.push_back(builtin);
  }
  for (const screen::Group& group : builtin_groups) {
    new_groups.push_back(group);
  }
  groups = std::move(new_groups);
  actions = std::move(new_actions);
  shown_dirty = true;
}

void read_choice(const sdk::Value& message, bool reset) {
  const std::string name = text_field(message, "name");
  if (!ours(name)) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  Choice& choice = choices[name];
  if (reset) {
    choice = Choice{};
    pending_keys[name] = {};  // the frame puts the default back
    for (const Wanted& wanted : actions) {
      if (wanted.action.name == name && !wanted.action.keyboard.empty()) {
        pending_keys[name] = {wanted.action.keyboard};
      }
    }
  } else {
    if (const sdk::Value* keys = list_field(message, "keys")) {
      choice.keyboard = inputs_of(*keys);
      pending_keys[name] = *choice.keyboard;
    }
    const sdk::Value* controller = kcd2s::integration::script_field(message, "controller");
    if (controller && controller->tag() == sdk::Tag::Str && plain(controller->as_str(), true)) {
      choice.controller = controller->as_str();
    }
  }
  if (!choice.keyboard && !choice.controller) {
    choices.erase(name);
  }
  save_choices(store, choices);
}

// The rows the resources hear; state_mutex held. With the game's screen, its current inputs.
std::vector<Row> rows_of(const std::map<std::string, screen::Binding>* screen_bindings) {
  std::vector<Row> rows;
  for (const Wanted& wanted : actions) {
    Row row;
    row.name = wanted.action.name;
    const auto chosen = choices.find(row.name);
    const auto shown = screen_bindings ? screen_bindings->find(row.name)
                                       : std::map<std::string, screen::Binding>::const_iterator{};
    if (screen_bindings && shown != screen_bindings->end()) {
      row.keys = shown->second.current;
    } else if (chosen != choices.end() && chosen->second.keyboard) {
      row.keys = *chosen->second.keyboard;
    } else if (!wanted.action.keyboard.empty()) {
      row.keys = {wanted.action.keyboard};
    }
    if (chosen != choices.end() && chosen->second.controller) {
      row.controller = *chosen->second.controller;
    } else {
      row.controller = wanted.controller;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

void publish(std::vector<Row> rows) {
  if (rows != report) {
    report = std::move(rows);
    ++report_revision;
  }
}

}  // namespace

void set_server(const std::string& server) {
  const std::wstring path = store_path(server);
  Choices loaded = load_choices(path);
  std::lock_guard<std::mutex> lock(state_mutex);
  store = path;
  choices = std::move(loaded);
  keep_only_builtins();
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string op = kcd2s::integration::script_operation(message);
  if (op == "keybinds") {
    read_set(message);
    return true;
  }
  if (op == "keybindSet") {
    read_choice(message, false);
    return true;
  }
  if (op == "keybindReset") {
    read_choice(message, true);
    return true;
  }
  return false;
}

void pump(const void* session, const RuntimeSender& send) {
  sdk::Value::Array rows;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (session != reported_session) {
      // A new runtime: it has asked for nothing yet, and the old one's actions leave the screen.
      reported_session = session;
      keep_only_builtins();
      report.clear();
      sent_revision = report_revision;
      return;
    }
    if (sent_revision == report_revision) {
      return;
    }
    for (const Row& row : report) {
      sdk::Value::Array keys;
      for (const std::string& key : row.keys) {
        keys.emplace_back(key);
      }
      rows.emplace_back(sdk::Value::Map{{"name", sdk::Value(row.name)},
                                        {"keys", sdk::Value(std::move(keys))},
                                        {"controller", sdk::Value(row.controller)}});
    }
  }
  sdk::Value::Map message{{"op", sdk::Value(std::string("keybindChoices"))},
                          {"actions", sdk::Value(std::move(rows))}};
  if (send(sdk::Value(std::move(message)))) {
    std::lock_guard<std::mutex> lock(state_mutex);
    sent_revision = report_revision;
  }
}

void add_builtin_group(const std::string& name, const std::string& label) {
  std::lock_guard<std::mutex> lock(state_mutex);
  for (const screen::Group& group : builtin_groups) {
    if (group.name == name) {
      return;
    }
  }
  builtin_groups.push_back(screen::Group{name, label});
  groups.push_back(screen::Group{name, label});
  hud::set_text(name, label);
  shown_dirty = true;
}

void add_builtin_action(const std::string& name, const std::string& group, const std::string& label,
                        const std::string& keyboard) {
  std::lock_guard<std::mutex> lock(state_mutex);
  for (const Wanted& builtin : builtin_actions) {
    if (builtin.action.name == name) {
      return;
    }
  }
  Wanted wanted;
  wanted.action.name = name;
  wanted.action.group = group;
  wanted.action.label = label;
  wanted.action.keyboard = keyboard;
  builtin_actions.push_back(wanted);
  actions.push_back(wanted);
  hud::set_text(name, label);
  shown_dirty = true;
}

std::vector<std::string> keys_of(const std::string& name) {
  std::lock_guard<std::mutex> lock(state_mutex);
  const auto chosen = choices.find(name);
  if (chosen != choices.end() && chosen->second.keyboard) {
    return *chosen->second.keyboard;
  }
  for (const Wanted& wanted : actions) {
    if (wanted.action.name == name && !wanted.action.keyboard.empty()) {
      return {wanted.action.keyboard};
    }
  }
  // A built-in action (voice chat's keys) not added yet still answers with its default.
  for (const Wanted& builtin : builtin_actions) {
    if (builtin.action.name == name && !builtin.action.keyboard.empty()) {
      return {builtin.action.keyboard};
    }
  }
  return {};
}

void frame() {
  if (!screen::ready()) {
    std::lock_guard<std::mutex> lock(state_mutex);
    publish(rows_of(nullptr));
    return;
  }
  bool show = false;
  std::vector<screen::Group> show_groups;
  std::vector<screen::Action> show_actions;
  std::map<std::string, std::vector<std::string>> keys;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (shown_dirty) {
      show = true;
      shown_dirty = false;
      show_groups = groups;
      for (const Wanted& wanted : actions) {
        show_actions.push_back(wanted.action);
        const auto chosen = choices.find(wanted.action.name);
        if (chosen != choices.end() && chosen->second.keyboard) {
          keys[wanted.action.name] = *chosen->second.keyboard;
        }
      }
    }
    for (auto& [name, inputs] : pending_keys) {
      keys[name] = std::move(inputs);
    }
    pending_keys.clear();
  }
  if (show) {
    screen::show(show_groups, show_actions);
  }
  screen::bind(keys);

  const ULONGLONG now = GetTickCount64();
  if (!show && keys.empty() && now - last_poll < kPollMs) {
    return;
  }
  last_poll = now;
  const std::map<std::string, screen::Binding> shown = screen::bindings();
  std::lock_guard<std::mutex> lock(state_mutex);
  // What the player set on the game's screen becomes their choice for this server.
  bool changed = false;
  for (const Wanted& wanted : actions) {
    const auto binding = shown.find(wanted.action.name);
    if (binding == shown.end()) {
      continue;
    }
    Choice& choice = choices[wanted.action.name];
    std::optional<std::vector<std::string>> keyboard;
    if (binding->second.current != binding->second.defaults) {
      keyboard = binding->second.current;
    }
    if (keyboard != choice.keyboard) {
      choice.keyboard = keyboard;
      changed = true;
    }
    if (!choice.keyboard && !choice.controller) {
      choices.erase(wanted.action.name);
    }
  }
  if (changed) {
    save_choices(store, choices);
  }
  publish(rows_of(&shown));
}

}  // namespace kcds::runtime::keybinds
