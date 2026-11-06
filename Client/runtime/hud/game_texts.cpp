#include "game_texts.hpp"

#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../branding/runtime_branding.hpp"

namespace kcds::runtime::hud {
namespace {

using Assign = void*(__fastcall*)(void* string, const char* text);

constexpr char kPrefix[] = "kcds_";

Assign assign = nullptr;
std::mutex texts_mutex;
// Lower-case key -> text.
std::unordered_map<std::string, std::string> texts;

std::string lower(const char* text) {
  std::string result(text);
  for (char& c : result) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

bool is_ours(const char* label) {
  return label[0] == '@' && _strnicmp(label + 1, kPrefix, sizeof(kPrefix) - 1) == 0;
}

bool kcds_text(const char* label, void* out) {
  if (!is_ours(label)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(texts_mutex);
  const auto it = texts.find(lower(label + 1));
  if (it == texts.end()) {
    return false;
  }
  assign(out, it->second.c_str());
  return true;
}

}  // namespace

bool install_game_texts(void* assign_text) {
  assign = reinterpret_cast<Assign>(assign_text);
  return assign && branding::add_label_override(kcds_text);
}

void set_text(const std::string& key, const std::string& text) {
  std::lock_guard<std::mutex> lock(texts_mutex);
  if (text.empty()) {
    texts.erase(lower(key.c_str()));
  } else {
    texts[lower(key.c_str())] = text;
  }
}

}  // namespace kcds::runtime::hud
