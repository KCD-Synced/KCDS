#include "bound_keys.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>

#include "key_places.hpp"
#include "keybinds.hpp"

namespace kcds::runtime::keybinds {

int virtual_key_of(const std::string& name) {
  if (const std::uint16_t scan = places::scan_code_of(name)) {
    return static_cast<int>(MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0)));
  }
  if (name.size() >= 2 && name[0] == 'f' && name[1] >= '0' && name[1] <= '9') {
    return VK_F1 + std::atoi(name.c_str() + 1) - 1;
  }
  struct Named {
    const char* name;
    int key;
  };
  static constexpr Named kNamed[] = {
      {"mouse3", VK_MBUTTON},   {"mouse4", VK_XBUTTON1}, {"mouse5", VK_XBUTTON2},
      {"capslock", VK_CAPITAL}, {"lalt", VK_LMENU},      {"ralt", VK_RMENU},
      {"lshift", VK_LSHIFT},    {"rshift", VK_RSHIFT},   {"lctrl", VK_LCONTROL},
      {"rctrl", VK_RCONTROL},   {"tab", VK_TAB},         {"space", VK_SPACE},
  };
  for (const Named& named : kNamed) {
    if (name == named.name) {
      return named.key;
    }
  }
  return 0;
}

bool held(const std::string& action) {
  for (const std::string& name : keys_of(action)) {
    const int key = virtual_key_of(name);
    if (key != 0 && (GetAsyncKeyState(key) & 0x8000) != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace kcds::runtime::keybinds
