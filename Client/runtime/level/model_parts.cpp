#include "model_parts.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>

#include "../props/prop_entity.hpp"

namespace kcds::runtime::level::parts {
namespace {

// IMaterial (CMatInfo) slots.
constexpr std::size_t kAddRef = 2;
constexpr std::size_t kRelease = 3;
constexpr std::size_t kName = 8;
constexpr std::size_t kSetSubCount = 27;
constexpr std::size_t kSubCount = 28;
constexpr std::size_t kSub = 30;
constexpr std::size_t kSetSub = 31;
// Material flags: not listed by name in the manager, and not drawn.
constexpr int kPureChild = 0x80;
constexpr int kNoDraw = 0x400;
// More parts than any level model has; a source with more is not copied.
constexpr int kMostParts = 256;
constexpr std::size_t kMostNameLength = 128;

using Call = void(__fastcall*)(void* material);
using Name = const char*(__fastcall*)(void* material);
using Count = int(__fastcall*)(void* material);
using SetCount = void(__fastcall*)(void* material, int count);
using Sub = void*(__fastcall*)(void* material, int index);
using SetSub = void(__fastcall*)(void* material, int index, void* sub);

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

// The one material every hidden part is drawn with: none. It keeps its reference for the game's
// whole run.
void* no_draw = nullptr;

void* no_draw_material() {
  if (!no_draw) {
    no_draw =
        kcds::runtime::props::entity::create_material("kcds_hidden_part", kPureChild | kNoDraw);
    if (no_draw) {
      slot<Call>(no_draw, kAddRef)(no_draw);
    }
  }
  return no_draw;
}

std::string lower_name(void* material) {
  const char* name = slot<Name>(material, kName)(material);
  std::string text = name ? std::string(name, strnlen(name, kMostNameLength)) : std::string();
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}

}  // namespace

void* without(void* source, const std::vector<std::string>& hidden, std::string& names,
              int& matched) {
  names.clear();
  matched = 0;
  if (!source) {
    return nullptr;
  }
  const int count = slot<Count>(source, kSubCount)(source);
  if (count <= 0 || count > kMostParts) {
    return nullptr;
  }
  void* hide = no_draw_material();
  void* copy = kcds::runtime::props::entity::create_material("kcds_model_parts", kPureChild);
  if (!hide || !copy) {
    return nullptr;
  }
  slot<Call>(copy, kAddRef)(copy);
  slot<SetCount>(copy, kSetSubCount)(copy, count);
  for (int index = 0; index < count; ++index) {
    void* part = slot<Sub>(source, kSub)(source, index);
    std::string name = part ? lower_name(part) : std::string();
    if (!names.empty()) {
      names += ", ";
    }
    names += name.empty() ? std::string("?") : name;
    const bool out = !name.empty() && std::find(hidden.begin(), hidden.end(), name) != hidden.end();
    if (out) {
      ++matched;
    }
    slot<SetSub>(copy, kSetSub)(copy, index, out ? hide : part);
  }
  return copy;
}

void hold(void* material) {
  if (material) {
    slot<Call>(material, kAddRef)(material);
  }
}

void release(void* material) {
  if (material) {
    slot<Call>(material, kRelease)(material);
  }
}

}  // namespace kcds::runtime::level::parts
