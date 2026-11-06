#include "splash.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// GDI+ expects min/max from windows.h, which NOMINMAX builds do not define.
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../testing/background_mode.hpp"
#include "MinHook.h"

namespace kcds::runtime::branding::splash {
namespace {

namespace gdi = Gdiplus;

// The exe's splash dialog and its picture control (resources of KingdomCome.exe 1.5.5).
constexpr WORD kSplashDialog = 231;
constexpr int kPictureControl = 1001;
constexpr WORD kSplashBitmap = 108;
constexpr const wchar_t* kOverlayRelativePath = L"assets\\kcds-splash-overlay.png";

using CreateDialogParamAFn = HWND(WINAPI*)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);

Logger log_line = nullptr;
std::wstring overlay_path;
CreateDialogParamAFn original_create_dialog = nullptr;
bool applied = false;

template <typename... Args>
void note(const char* format, Args... args) {
  if (log_line) {
    log_line(format, args...);
  }
}

bool is_splash(HINSTANCE instance, LPCSTR name) {
  return instance == GetModuleHandleW(nullptr) && IS_INTRESOURCE(name) &&
         LOWORD(reinterpret_cast<ULONG_PTR>(name)) == kSplashDialog;
}

// A test game's splash (testing/background_mode.hpp): made from a copy of the exe's dialog
// template without WS_VISIBLE, then shown without activating, so it never takes the foreground
// (user32 shows and activates a visible dialog itself as it creates it). Null when the template
// is not found.
HWND create_inactive(HINSTANCE instance, LPCSTR name, HWND parent, DLGPROC procedure,
                     LPARAM parameter) {
  HRSRC found = FindResourceA(instance, name, MAKEINTRESOURCEA(5));  // RT_DIALOG
  HGLOBAL loaded = found ? LoadResource(instance, found) : nullptr;
  const auto* bytes = loaded ? static_cast<const std::uint8_t*>(LockResource(loaded)) : nullptr;
  const DWORD size = found ? SizeofResource(instance, found) : 0;
  if (bytes == nullptr || size < 16) {
    return nullptr;
  }
  // DWORD aligned, as a dialog template must be.
  std::vector<DWORD> copy((size + sizeof(DWORD) - 1) / sizeof(DWORD));
  std::memcpy(copy.data(), bytes, size);
  auto* raw = reinterpret_cast<std::uint8_t*>(copy.data());
  WORD version = 0;
  WORD signature = 0;
  std::memcpy(&version, raw, sizeof(version));
  std::memcpy(&signature, raw + 2, sizeof(signature));
  // DLGTEMPLATEEX (version 1, signature 0xFFFF) has its style at + 12, DLGTEMPLATE at + 0.
  const std::size_t style_at = version == 1 && signature == 0xFFFF ? 12 : 0;
  DWORD style = 0;
  std::memcpy(&style, raw + style_at, sizeof(style));
  style &= ~static_cast<DWORD>(WS_VISIBLE);
  std::memcpy(raw + style_at, &style, sizeof(style));
  HWND dialog = CreateDialogIndirectParamA(instance, reinterpret_cast<LPCDLGTEMPLATEA>(raw), parent,
                                           procedure, parameter);
  if (dialog != nullptr) {
    ShowWindow(dialog, SW_SHOWNA);
  }
  return dialog;
}

// The exe's splash bitmap with our overlay drawn over it, as a GDI bitmap the picture control
// can own. Null when the bitmap or the overlay cannot be loaded.
HBITMAP compose_splash() {
  gdi::GdiplusStartupInput input;
  ULONG_PTR token = 0;
  if (gdi::GdiplusStartup(&token, &input, nullptr) != gdi::Ok) {
    note("SPLASH GDI+ unavailable; original splash");
    return nullptr;
  }
  HBITMAP result = nullptr;
  HBITMAP original =
      static_cast<HBITMAP>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kSplashBitmap),
                                      IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
  if (original == nullptr) {
    note("SPLASH bitmap %u not found in the exe; original splash", kSplashBitmap);
  } else {
    {
      std::unique_ptr<gdi::Bitmap> base(gdi::Bitmap::FromHBITMAP(original, nullptr));
      gdi::Bitmap overlay(overlay_path.c_str());
      if (!base || base->GetLastStatus() != gdi::Ok || overlay.GetLastStatus() != gdi::Ok) {
        note("SPLASH overlay not loaded; original splash");
      } else {
        const INT width = static_cast<INT>(base->GetWidth());
        const INT height = static_cast<INT>(base->GetHeight());
        gdi::Bitmap canvas(width, height, PixelFormat24bppRGB);
        {
          gdi::Graphics graphics(&canvas);
          graphics.SetInterpolationMode(gdi::InterpolationModeHighQualityBicubic);
          graphics.DrawImage(base.get(), 0, 0, width, height);
          // The overlay is made for the splash size; it is scaled when the exe's bitmap differs.
          graphics.DrawImage(&overlay, 0, 0, width, height);
        }
        if (canvas.GetHBITMAP(gdi::Color(0, 0, 0), &result) != gdi::Ok) {
          result = nullptr;
        }
      }
    }
    DeleteObject(original);
  }
  gdi::GdiplusShutdown(token);
  return result;
}

// The exe's dialog is wider than its picture, which left the dialog background showing as a
// strip on the right. The picture control and the dialog take the bitmap's size;
// the dialog keeps the centre the game gave it.
void fit_to_bitmap(HWND dialog, HWND picture, HBITMAP bitmap) {
  BITMAP info{};
  if (GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight <= 0) {
    return;
  }
  RECT window{};
  RECT client{};
  if (!GetWindowRect(dialog, &window) || !GetClientRect(dialog, &client)) {
    return;
  }
  // Borders, if the dialog has any, stay as they are around the new client area.
  const int frame_width = (window.right - window.left) - (client.right - client.left);
  const int frame_height = (window.bottom - window.top) - (client.bottom - client.top);
  const int width = info.bmWidth + frame_width;
  const int height = info.bmHeight + frame_height;
  const int left = (window.left + window.right - width) / 2;
  const int top = (window.top + window.bottom - height) / 2;
  SetWindowPos(picture, nullptr, 0, 0, info.bmWidth, info.bmHeight, SWP_NOZORDER | SWP_NOACTIVATE);
  SetWindowPos(dialog, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  note("SPLASH fitted to %dx%d (was %ldx%ld)", info.bmWidth, info.bmHeight,
       client.right - client.left, client.bottom - client.top);
}

HWND WINAPI hooked_create_dialog(HINSTANCE instance, LPCSTR name, HWND parent, DLGPROC procedure,
                                 LPARAM parameter) {
  HWND dialog = nullptr;
  if (background::enabled() && !applied && is_splash(instance, name)) {
    dialog = create_inactive(instance, name, parent, procedure, parameter);
  }
  if (dialog == nullptr) {
    dialog = original_create_dialog(instance, name, parent, procedure, parameter);
  }
  if (dialog == nullptr || applied || !is_splash(instance, name)) {
    return dialog;
  }
  applied = true;
  HWND picture = GetDlgItem(dialog, kPictureControl);
  if (picture == nullptr) {
    note("SPLASH picture control missing; original splash");
    return dialog;
  }
  HBITMAP splash = compose_splash();
  if (splash == nullptr) {
    return dialog;
  }
  // The control keeps showing our bitmap; it lives as long as the splash (a few seconds).
  SendMessageW(picture, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(splash));
  fit_to_bitmap(dialog, picture, splash);
  InvalidateRect(dialog, nullptr, TRUE);
  note("SPLASH KCD2-Synced splash shown");
  return dialog;
}

}  // namespace

bool install(const std::wstring& install_directory, Logger logger) {
  log_line = logger;
  overlay_path = install_directory + kOverlayRelativePath;
  // A test game's splash is still made inactive without the overlay (its picture stays the
  // exe's own then).
  const bool overlay_missing = GetFileAttributesW(overlay_path.c_str()) == INVALID_FILE_ATTRIBUTES;
  if (overlay_missing && !background::enabled()) {
    note("SPLASH overlay missing; original splash");
    return false;
  }
  void* target = nullptr;
  if (MH_CreateHookApiEx(L"user32", "CreateDialogParamA",
                         reinterpret_cast<void*>(hooked_create_dialog),
                         reinterpret_cast<void**>(&original_create_dialog), &target) != MH_OK) {
    note("SPLASH dialog hook could not be created; original splash");
    return false;
  }
  if (MH_EnableHook(target) != MH_OK) {
    MH_RemoveHook(target);
    note("SPLASH dialog hook could not be enabled; original splash");
    return false;
  }
  note("SPLASH dialog hook installed");
  return true;
}

}  // namespace kcds::runtime::branding::splash
