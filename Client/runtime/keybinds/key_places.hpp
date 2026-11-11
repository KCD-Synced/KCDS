// Where an engine input name sits on the keyboard. The engine names keys by their place on a US
// keyboard ("b" is the key right of "v", whatever the layout prints on it), so a binding keeps its
// place on every layout. Letters and digits give that place's scan code; other names have a fixed
// virtual key (keys the layout does not move: function keys, mouse buttons, modifiers).
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::keybinds::places {

// The set-1 scan code of a letter or digit name ("a" .. "z", "0" .. "9"), 0 for any other name.
inline std::uint16_t scan_code_of(const std::string& name) {
  // The letters a .. z in order, by their place on a US keyboard.
  static constexpr std::uint16_t kLetters[26] = {
      0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
      0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
  if (name.size() != 1) {
    return 0;
  }
  const char key = name[0];
  if (key >= 'a' && key <= 'z') {
    return kLetters[key - 'a'];
  }
  if (key >= '1' && key <= '9') {
    return static_cast<std::uint16_t>(0x02 + (key - '1'));
  }
  if (key == '0') {
    return 0x0B;
  }
  return 0;
}

}  // namespace kcds::runtime::keybinds::places
