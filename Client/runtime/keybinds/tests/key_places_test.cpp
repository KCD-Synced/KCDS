#include "../key_places.hpp"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using kcds::runtime::keybinds::places::scan_code_of;
  expect(scan_code_of("b") == 0x30, "b is right of v");
  expect(scan_code_of("a") == 0x1E, "a");
  expect(scan_code_of("z") == 0x2C, "z (y on a German keyboard)");
  expect(scan_code_of("q") == 0x10, "q");
  expect(scan_code_of("1") == 0x02, "1");
  expect(scan_code_of("9") == 0x0A, "9");
  expect(scan_code_of("0") == 0x0B, "0");
  expect(scan_code_of("f5") == 0, "a function key has no place here");
  expect(scan_code_of("mouse4") == 0, "nor a mouse button");
  expect(scan_code_of("") == 0, "nor nothing");
  expect(scan_code_of("B") == 0, "engine names are lower case");
  if (failures == 0) {
    std::printf("key_places: all passed\n");
  }
  return failures == 0 ? 0 : 1;
}
