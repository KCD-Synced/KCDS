#include "window_mode_memory.hpp"

#include <cstdlib>

namespace kcds::runtime::display {
namespace {

constexpr const char* kBorderlessKey = "\"borderless\": ";
constexpr const char* kVanillaKey = "\"vanilla\": ";

// A mode a game without KCDS can show in its menu; the borderless window is not one.
bool is_vanilla_mode(int window_type) {
  return window_type == kWindowed || window_type == kBorderlessFullScreen ||
         window_type == kExclusiveFullScreen;
}

}  // namespace

ProfileSave on_profile_save(int window_type, const WindowModeMemory& memory) {
  ProfileSave result;
  if (window_type == kBorderlessWindow) {
    result.memory.borderless = true;
    result.memory.vanilla = memory.vanilla;
    result.saved = memory.vanilla;
    return result;
  }
  result.memory.borderless = false;
  result.memory.vanilla = is_vanilla_mode(window_type) ? window_type : memory.vanilla;
  result.saved = window_type;
  return result;
}

GameStart on_game_start(int window_type, const WindowModeMemory& memory) {
  GameStart result;
  result.memory = memory;
  // A profile written by an older client may hold the borderless window itself; the vanilla mode
  // is then the one remembered.
  if (is_vanilla_mode(window_type)) {
    result.memory.vanilla = window_type;
  }
  if (memory.borderless) {
    result.window_type = kBorderlessWindow;
  } else {
    result.window_type = result.memory.vanilla;
  }
  return result;
}

std::string to_text(const WindowModeMemory& memory) {
  std::string text = "{\n  ";
  text += kBorderlessKey;
  text += memory.borderless ? "true" : "false";
  text += ",\n  ";
  text += kVanillaKey;
  text += std::to_string(memory.vanilla);
  text += "\n}\n";
  return text;
}

WindowModeMemory from_text(const std::string& text) {
  WindowModeMemory memory;
  const std::string borderless_key = kBorderlessKey;
  const auto borderless_at = text.find(borderless_key);
  if (borderless_at != std::string::npos) {
    memory.borderless = text.compare(borderless_at + borderless_key.size(), 4, "true") == 0;
  }
  const std::string vanilla_key = kVanillaKey;
  const auto vanilla_at = text.find(vanilla_key);
  if (vanilla_at != std::string::npos) {
    const int vanilla = std::atoi(text.c_str() + vanilla_at + vanilla_key.size());
    if (is_vanilla_mode(vanilla)) {
      memory.vanilla = vanilla;
    }
  }
  return memory;
}

}  // namespace kcds::runtime::display
