#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "keybind_screen.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>

#include "MinHook.h"

namespace kcds::runtime::keybinds::screen {
namespace {

// ISystem from the services: (services + 8)->vtable + 152; LoadXmlFromBuffer at vtable + 1040.
constexpr std::size_t kSystemHolder = 8;
constexpr std::size_t kGetSystem = 152 / 8;
constexpr std::size_t kLoadXmlFromBuffer = 1040 / 8;
// The services hold C_PlayerModule at + 296; C_PlayerModule holds C_Keybinds at + 64
// (builds the modules; the player module's first system event loads the bindings).
constexpr std::size_t kPlayerModule = 296;
constexpr std::size_t kModuleKeybinds = 64;
// IXmlNode::Release.
constexpr std::size_t kRelease = 32 / 8;
// C_Keybinds: the list heads of its superaction and group maps.
constexpr std::size_t kSuperactionList = 24;
constexpr std::size_t kGroupList = 88;
// A list node: its name (a CryString) and its order on the screen.
constexpr std::size_t kNodeName = 24;
constexpr std::size_t kNodeOrder = 40;
// A superaction node, per device 80 bytes apart: access (0 hidden, 1 readable, 2 writeable),
// default inputs, current inputs (vectors of CryString, 24 bytes).
constexpr std::size_t kAccess = 80;
constexpr std::size_t kDefaults = 88;
constexpr std::size_t kCurrent = 112;
constexpr std::size_t kDeviceStride = 80;
constexpr int kDevices = 3;
constexpr int kWriteable = 2;
// A list longer than this is not the game's (it has some 80 superactions and 8 groups).
constexpr int kMaxNodes = 4096;
constexpr char kPrefix[] = "kcds_";

using Parse = std::int64_t(__fastcall*)(void* keybinds, void** tree);
using Apply = std::int64_t(__fastcall*)(void* keybinds, void** tree);
using Save = std::int64_t(__fastcall*)(void* serializer, void* profile);
using GetSystem = void*(__fastcall*)(void* holder);
using LoadXml = void**(__fastcall*)(void* system, void** out, const char* buffer,
                                    std::size_t length, bool reuse_strings, bool quiet);
using Release = void(__fastcall*)(void* node);

Natives natives;
Logger log_line = nullptr;
Parse original_parse = nullptr;
Apply original_apply = nullptr;
Save original_save = nullptr;
void* keybinds_object = nullptr;
bool logged_layout = false;

template <class Function>
Function slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}

template <class T>
T read(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
  return value;
}

void write_int(void* base, std::size_t offset, int value) {
  std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

bool is_ours(const char* name) {
  return name && std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) == 0;
}

// The nodes of one of C_Keybinds' lists, in list order.
std::vector<void*> nodes_of(std::size_t list) {
  std::vector<void*> nodes;
  if (!keybinds_object) {
    return nodes;
  }
  void* head = read<void*>(keybinds_object, list);
  if (!head) {
    return nodes;
  }
  void* node = read<void*>(head, 0);
  while (node && node != head && static_cast<int>(nodes.size()) < kMaxNodes) {
    nodes.push_back(node);
    node = read<void*>(node, 0);
  }
  return nodes;
}

std::string name_of(const void* node) {
  const char* name = read<const char*>(node, kNodeName);
  return name ? std::string(name) : std::string();
}

// A vector of CryString at this address.
std::vector<std::string> inputs_at(const void* node, std::size_t offset) {
  std::vector<std::string> inputs;
  const auto begin = read<const char* const*>(node, offset);
  const auto end = read<const char* const*>(node, offset + 8);
  if (!begin || end < begin || end - begin > 16) {
    return inputs;
  }
  for (auto it = begin; it != end; ++it) {
    if (*it && **it) {
      inputs.emplace_back(*it);
    }
  }
  return inputs;
}

void* system_of() {
  void* services = natives.services ? natives.services() : nullptr;
  void* holder = services ? read<void*>(services, kSystemHolder) : nullptr;
  return holder ? slot_of<GetSystem>(holder, kGetSystem)(holder) : nullptr;
}

// Parses an XML text into a node (one reference, released by the caller), or null.
void* load_xml(const std::string& text) {
  void* system = system_of();
  if (!system) {
    return nullptr;
  }
  void* root = nullptr;
  slot_of<LoadXml>(system, kLoadXmlFromBuffer)(system, &root, text.data(), text.size(), false,
                                               true);
  return root;
}

void release(void* node) {
  if (node) {
    slot_of<Release>(node, kRelease)(node);
  }
}

// Numbers the KCDS nodes of a list after the game's: the wanted ones in their order, then the
// others. The game's numbers stay as they are.
void renumber(std::size_t list, const std::vector<std::string>& wanted) {
  std::vector<void*> ours;
  int game_last = -1;
  int game_count = 0;
  for (void* node : nodes_of(list)) {
    const std::string name = name_of(node);
    if (is_ours(name.c_str())) {
      ours.push_back(node);
    } else {
      game_last = std::max(game_last, read<int>(node, kNodeOrder));
      ++game_count;
    }
  }
  if (!logged_layout && log_line) {
    log_line("KEYBINDS list +%zu: %d game entries (last order %d), %zu ours", list, game_count,
             game_last, ours.size());
  }
  int next = game_last + 1;
  std::set<void*> numbered;
  for (const std::string& name : wanted) {
    for (void* node : ours) {
      if (numbered.count(node) == 0 && name_of(node) == name) {
        write_int(node, kNodeOrder, next++);
        numbered.insert(node);
      }
    }
  }
  for (void* node : ours) {
    if (numbered.count(node) == 0) {
      write_int(node, kNodeOrder, next++);
    }
  }
}

// Runs the game's apply on an XML text of <superaction name controller><control input slot/>.
void apply_text(const std::string& text) {
  void* root = load_xml(text);
  if (!root) {
    if (log_line) {
      log_line("KEYBINDS the bindings could not be read as XML");
    }
    return;
  }
  original_apply(keybinds_object, &root);
  release(root);
}

std::int64_t __fastcall hooked_parse(void* keybinds, void** tree) {
  keybinds_object = keybinds;
  return original_parse(keybinds, tree);
}

std::int64_t __fastcall hooked_apply(void* keybinds, void** tree) {
  keybinds_object = keybinds;
  return original_apply(keybinds, tree);
}

// The game's profile save: KCDS actions read as unchanged, so none of them is written.
std::int64_t __fastcall hooked_save(void* serializer, void* profile) {
  keybinds_object = static_cast<std::uint8_t*>(serializer) - 8;
  struct Kept {
    std::uint8_t* at;
    std::uint8_t bytes[24];
  };
  std::vector<Kept> kept;
  for (void* node : nodes_of(kSuperactionList)) {
    if (!is_ours(name_of(node).c_str())) {
      continue;
    }
    auto* base = static_cast<std::uint8_t*>(node);
    for (int device = 0; device < kDevices; ++device) {
      const std::size_t step = kDeviceStride * std::size_t(device);
      Kept entry{base + kCurrent + step, {}};
      std::memcpy(entry.bytes, entry.at, sizeof(entry.bytes));
      std::memcpy(entry.at, base + kDefaults + step, sizeof(entry.bytes));
      kept.push_back(entry);
    }
  }
  const std::int64_t result = original_save(serializer, profile);
  for (const Kept& entry : kept) {
    std::memcpy(entry.at, entry.bytes, sizeof(entry.bytes));
  }
  return result;
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.services) {
    return false;
  }
  const bool parse = hook(natives.parse, reinterpret_cast<void*>(&hooked_parse),
                          reinterpret_cast<void**>(&original_parse));
  const bool apply = hook(natives.apply, reinterpret_cast<void*>(&hooked_apply),
                          reinterpret_cast<void**>(&original_apply));
  const bool save = hook(natives.save, reinterpret_cast<void*>(&hooked_save),
                         reinterpret_cast<void**>(&original_save));
  return parse && apply && save;
}

bool ready() {
  if (keybinds_object) {
    return true;
  }
  // Injected after the game read its bindings: C_Keybinds through the player module, once its
  // list holds the game's actions.
  void* services = natives.services ? natives.services() : nullptr;
  void* module = services ? read<void*>(services, kPlayerModule) : nullptr;
  void* keybinds = module ? read<void*>(module, kModuleKeybinds) : nullptr;
  if (!keybinds) {
    return false;
  }
  void* head = read<void*>(keybinds, kSuperactionList);
  if (!head || read<void*>(head, 0) == head) {
    return false;
  }
  keybinds_object = keybinds;
  if (log_line) {
    log_line("KEYBINDS C_Keybinds found through the player module");
  }
  return true;
}

void show(const std::vector<Group>& groups, const std::vector<Action>& actions) {
  if (!keybinds_object) {
    return;
  }
  std::set<std::string> wanted;
  for (const Action& action : actions) {
    wanted.insert(action.name);
  }
  std::vector<std::string> shown_before;
  std::vector<std::string> game_writeable;
  for (void* node : nodes_of(kSuperactionList)) {
    const std::string name = name_of(node);
    if (is_ours(name.c_str())) {
      shown_before.push_back(name);
    } else if (read<int>(node, kAccess) == kWriteable) {
      game_writeable.push_back(name);
    }
  }
  // Their current inputs leave the game's input maps first; the parse adds the defaults again.
  if (!shown_before.empty()) {
    std::string clear = "<keybinds>";
    for (const std::string& name : shown_before) {
      clear += "<superaction name=\"" + name + "\" controller=\"keyboard\"/>";
    }
    clear += "</keybinds>";
    apply_text(clear);
  }

  std::string text = "<keybinds>";
  for (const Group& group : groups) {
    text += "<ui_group name=\"" + group.name + "\" ui_label=\"" + group.name + "\"/>";
  }
  for (const Action& action : actions) {
    text += "<superaction name=\"" + action.name + "\" ui_group=\"" + action.group +
            "\" ui_name=\"" + action.name + "\" ui_tooltip=\"\" keyboard=\"writeable\">";
    text += "<control input=\"" + action.keyboard + "\" controller=\"keyboard\" slot=\"0\"/>";
    text += "</superaction>";
  }
  for (const std::string& name : shown_before) {
    if (wanted.count(name) == 0) {
      text += "<superaction name=\"" + name +
              "\" ui_group=\"other\" ui_name=\"\" ui_tooltip=\"\" keyboard=\"hidden\">";
      text += "<control input=\"\" controller=\"keyboard\" slot=\"0\"/></superaction>";
    }
  }
  // A key of a KCDS action may not also be one of the game's actions or another KCDS action.
  if (!actions.empty()) {
    text += "<conflict name=\"kcds\">";
    for (const Action& action : actions) {
      text += "<superaction name=\"" + action.name + "\"/>";
    }
    for (const std::string& name : game_writeable) {
      text += "<superaction name=\"" + name + "\"/>";
    }
    text += "</conflict>";
  }
  text += "</keybinds>";

  void* root = load_xml(text);
  if (!root) {
    if (log_line) {
      log_line("KEYBINDS the actions could not be read as XML");
    }
    return;
  }
  original_parse(keybinds_object, &root);
  release(root);

  std::vector<std::string> group_names;
  for (const Group& group : groups) {
    group_names.push_back(group.name);
  }
  std::vector<std::string> action_names;
  for (const Action& action : actions) {
    action_names.push_back(action.name);
  }
  renumber(kSuperactionList, action_names);
  renumber(kGroupList, group_names);
  logged_layout = true;
  if (log_line) {
    log_line("KEYBINDS %zu actions in %zu own groups on the screen, %zu hidden", actions.size(),
             groups.size(), shown_before.size() - std::min(shown_before.size(), wanted.size()));
  }
}

void bind(const std::map<std::string, std::vector<std::string>>& inputs) {
  if (!keybinds_object || inputs.empty()) {
    return;
  }
  std::string text = "<keybinds>";
  for (const auto& [name, list] : inputs) {
    text += "<superaction name=\"" + name + "\" controller=\"keyboard\">";
    for (std::size_t slot = 0; slot < list.size(); ++slot) {
      text += "<control input=\"" + list[slot] + "\" slot=\"" + std::to_string(slot) + "\"/>";
    }
    text += "</superaction>";
  }
  text += "</keybinds>";
  apply_text(text);
}

std::map<std::string, Binding> bindings() {
  std::map<std::string, Binding> result;
  for (void* node : nodes_of(kSuperactionList)) {
    const std::string name = name_of(node);
    if (!is_ours(name.c_str())) {
      continue;
    }
    Binding binding;
    binding.current = inputs_at(node, kCurrent);
    binding.defaults = inputs_at(node, kDefaults);
    result.emplace(name, std::move(binding));
  }
  return result;
}

}  // namespace kcds::runtime::keybinds::screen
