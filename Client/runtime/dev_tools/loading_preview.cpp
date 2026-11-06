// kcds-loading-preview <folder>: renders the KCDS loading screen (branding/loading_screen.cpp)
// into PNGs at the window sizes players have, at a few moments of its animation, so a
// design change is checked without starting the game.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>

#include "../branding/loading_screen.hpp"

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::fwprintf(stderr, L"usage: kcds-loading-preview <folder>\n");
    return 2;
  }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct Size {
    unsigned width;
    unsigned height;
    const wchar_t* name;
  };
  const Size sizes[] = {{1280, 720, L"720p"},       {1920, 1080, L"1080p"}, {2560, 1440, L"1440p"},
                        {3840, 2160, L"2160p"},     {1920, 1200, L"16x10"}, {2560, 1080, L"21x9"},
                        {3440, 1440, L"3440x1440"}, {5120, 1440, L"32x9"}};
  const double moments[] = {0.0, 550.0, 850.0, 1100.0, 1650.0};
  int failures = 0;
  for (const Size& size : sizes) {
    for (const double ms : moments) {
      const std::wstring path = std::wstring(argv[1]) + L"\\loading-" + size.name + L"-" +
                                std::to_wstring(static_cast<int>(ms)) + L"ms.png";
      if (!kcds::runtime::branding::loading_screen::write_preview(path.c_str(), size.width,
                                                                  size.height, ms)) {
        std::fwprintf(stderr, L"failed: %ls\n", path.c_str());
        ++failures;
      }
    }
  }
  CoUninitialize();
  return failures == 0 ? 0 : 1;
}
