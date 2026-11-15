// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "cursor_style.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "../core/log.hpp"
#include "../native_ui/native_ui.hpp"
#include "../session/resource_cache.hpp"
#include "../web/resource_web.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "resource_input.hpp"

namespace kcds::runtime::input::cursor_style {
namespace {

using Microsoft::WRL::ComPtr;
namespace sdk = kcd2s::sdk;

// The largest picture and file taken for a pointer.
constexpr UINT kMostSide = 256;
constexpr std::uintmax_t kMostBytes = 1u << 20;

enum class Mode { Game, Hidden, Image };

std::mutex mutex;
Mode mode = Mode::Game;
HCURSOR picture = nullptr;

std::string text_of(const sdk::Value& message, const char* name) {
  const sdk::Value* field = kcd2s::integration::script_field(message, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

int number_of(const sdk::Value& message, const char* name) {
  const sdk::Value* field = kcd2s::integration::script_field(message, name);
  if (field && field->tag() == sdk::Tag::Int) {
    return int(field->as_int());
  }
  if (field && field->tag() == sdk::Tag::Double) {
    return int(field->as_double());
  }
  return 0;
}

std::string from_base64(const std::string& text) {
  static const std::string kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(text.size() * 3 / 4);
  unsigned buffer = 0;
  int bits = 0;
  for (const char c : text) {
    const auto at = kAlphabet.find(c);
    if (at == std::string::npos) {
      continue;
    }
    buffer = (buffer << 6) | unsigned(at);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(char((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

// A Windows cursor of a PNG (or any picture WIC reads), its hotspot in pixels from the top left;
// null when it cannot be read or is larger than kMostSide.
HCURSOR cursor_of(const std::string& bytes, int hotspot_x, int hotspot_y) {
  if (bytes.empty() || bytes.size() > kMostBytes) {
    return nullptr;
  }
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HCURSOR made = nullptr;
  {
    ComPtr<IWICImagingFactory> imaging;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width = 0;
    UINT height = 0;
    const bool decoded =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&imaging))) &&
        SUCCEEDED(imaging->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), DWORD(bytes.size()))) &&
        SUCCEEDED(imaging->CreateDecoderFromStream(stream.Get(), nullptr,
                                                   WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) &&
        width > 0 && height > 0 && width <= kMostSide && height <= kMostSide &&
        SUCCEEDED(imaging->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom));
    if (decoded) {
      // A top-down 32-bit colour bitmap with its alpha, and an empty mask: the colour's alpha
      // shapes the pointer.
      BITMAPV5HEADER header{};
      header.bV5Size = sizeof(header);
      header.bV5Width = LONG(width);
      header.bV5Height = -LONG(height);
      header.bV5Planes = 1;
      header.bV5BitCount = 32;
      header.bV5Compression = BI_BITFIELDS;
      header.bV5RedMask = 0x00FF0000;
      header.bV5GreenMask = 0x0000FF00;
      header.bV5BlueMask = 0x000000FF;
      header.bV5AlphaMask = 0xFF000000;
      void* pixels = nullptr;
      HDC screen = GetDC(nullptr);
      HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                        DIB_RGB_COLORS, &pixels, nullptr, 0);
      ReleaseDC(nullptr, screen);
      HBITMAP mask = CreateBitmap(int(width), int(height), 1, 1, nullptr);
      if (colour && mask && pixels &&
          SUCCEEDED(converter->CopyPixels(nullptr, width * 4, width * height * 4,
                                          static_cast<BYTE*>(pixels)))) {
        ICONINFO info{};
        info.fIcon = FALSE;
        info.xHotspot =
            DWORD(hotspot_x < 0 ? 0 : (UINT(hotspot_x) >= width ? width - 1 : hotspot_x));
        info.yHotspot =
            DWORD(hotspot_y < 0 ? 0 : (UINT(hotspot_y) >= height ? height - 1 : hotspot_y));
        info.hbmMask = mask;
        info.hbmColor = colour;
        made = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
      }
      if (colour) {
        DeleteObject(colour);
      }
      if (mask) {
        DeleteObject(mask);
      }
    }
  }
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return made;
}

// The bytes of the picture: sent along, or a big file in the download cache.
std::string picture_bytes(const sdk::Value& message) {
  const std::string cached = text_of(message, "cached");
  if (cached.empty()) {
    return from_base64(text_of(message, "data"));
  }
  const std::filesystem::path source = kcds::runtime::session::cached_resource_file(cached);
  std::error_code error;
  const auto size = source.empty() ? 0 : std::filesystem::file_size(source, error);
  if (source.empty() || error || size == 0 || size > kMostBytes) {
    return {};
  }
  std::string bytes(std::size_t(size), '\0');
  std::ifstream in(source, std::ios::binary);
  if (!in.read(bytes.data(), std::streamsize(size))) {
    return {};
  }
  return bytes;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "cursorStyle") {
    return false;
  }
  const std::string wanted = text_of(message, "mode");
  Mode next = Mode::Game;
  HCURSOR made = nullptr;
  if (wanted == "hidden") {
    next = Mode::Hidden;
  } else if (wanted == "image") {
    made = cursor_of(picture_bytes(message), number_of(message, "hotspotX"),
                     number_of(message, "hotspotY"));
    // A picture that cannot be read leaves the game's pointer.
    next = made ? Mode::Image : Mode::Game;
  }
  HCURSOR old = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex);
    mode = next;
    old = picture;
    picture = made;
  }
  // The old picture goes once the window has the new one: a cursor in use is not destroyed.
  if (old) {
    if (GetCursor() == old) {
      SetCursor(made);
    }
    DestroyCursor(old);
  }
  kcds::game_client::log("CURSOR style %s%s", wanted.c_str(),
                         wanted == "image" && !made ? " (picture unreadable: the game's)" : "");
  return true;
}

bool set_cursor(unsigned hit_test) {
  if (hit_test != HTCLIENT || !controls_cursor() || kcds::runtime::web::has_input_focus() ||
      kcds::runtime::native_ui::world_covered_by_screen()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex);
  if (mode == Mode::Game) {
    return false;
  }
  SetCursor(mode == Mode::Hidden ? nullptr : picture);
  return true;
}

}  // namespace kcds::runtime::input::cursor_style
