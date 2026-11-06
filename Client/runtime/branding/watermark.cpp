#include "watermark.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "../architect/editor_overlay.hpp"
#include "../hud/hud_text.hpp"

namespace kcds::runtime::branding::watermark {
namespace {

using Microsoft::WRL::ComPtr;

// The mark's PNG, generated at configure time from branding/assets/kcds-watermark-mark.png.
constexpr unsigned char kMarkPng[] = {
#include "kcds_watermark_mark.inc"
};

constexpr wchar_t kName[] = L"KCD2 SYNCED";
constexpr wchar_t kLocalhost[] = L"localhost";
constexpr wchar_t kDot[] = L" · ";

// The design's sizes at a 1080-pixel-high frame (the design canvas, In-game HUD: the watermark),
// scaled with the frame's height: the mark's side, the gap to the text, the name
// (Cinzel 700, 12 px, 3 px apart) and under it the server line (EB Garamond 14 px), 44 px from the
// left edge and 36 px from the bottom.
constexpr float kReferenceHeight = 1080.0f;
constexpr float kMarkSide = 30.0f;
constexpr float kGap = 10.0f;
constexpr float kNameSize = 12.0f;
constexpr float kNameSpacing = 3.0f;
constexpr float kLineSize = 14.0f;
constexpr float kLineGap = 1.0f;
constexpr float kLeft = 44.0f;
constexpr float kBottom = 36.0f;
// The design's colours: the name, the server, the dot between and the ping.
constexpr std::uint32_t kNameColor = 0xE9DCBC;
constexpr std::uint32_t kServerColor = 0xCFC3A8;
constexpr std::uint32_t kDotColor = 0x8A7F6B;
constexpr std::uint32_t kPingColor = 0xB6CF8E;
// In World Architect's editor: the logo at the left end of its menu bar (36 pixels high at 1080),
// from x 12 of the editor's stage, which fills the frame; the name alone beside it.
constexpr float kEditorLeft = 12.0f;
constexpr float kEditorTop = 7.0f;
constexpr float kEditorMarkSide = 22.0f;
constexpr float kEditorGap = 8.0f;

// What the watermark draws with, made for one canvas (the overlay makes its canvas once).
struct Resources {
  ID2D1DeviceContext* canvas = nullptr;
  ComPtr<ID2D1Bitmap> mark;
  ComPtr<ID2D1SolidColorBrush> brush;
};

// A line laid out once for its text and size.
struct Line {
  std::wstring text;
  float size = 0.0f;
  ComPtr<IDWriteTextLayout> layout;
  DWRITE_TEXT_METRICS metrics{};
};

Resources resources;
Line name_line;
Line server_line;
Line dot_line;
Line ping_line;
// The player is in a loaded world (set_in_world).
std::atomic<bool> shown{false};
// World Architect's editor is open (set_editor).
std::atomic<bool> editor{false};
// The frame's right end of the watermark, in pixels (right_edge).
std::atomic<float> painted_right{0.0f};
// The height of its middle on that frame, in pixels from the top (middle).
std::atomic<float> painted_middle{0.0f};
// The server this game plays on (set_server, set_ping).
std::mutex server_mutex;
std::wstring server_name;
bool server_local = false;
int ping_ms = -1;

D2D1_COLOR_F color_of(std::uint32_t rgb, float alpha) {
  return D2D1::ColorF(float((rgb >> 16) & 0xFF) / 255.0f, float((rgb >> 8) & 0xFF) / 255.0f,
                      float(rgb & 0xFF) / 255.0f, alpha);
}

// The mark decoded from its PNG into a bitmap of the canvas; null when it cannot be (the name is
// drawn without it then).
ComPtr<ID2D1Bitmap> decode_mark(ID2D1DeviceContext* canvas) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<ID2D1Bitmap> bitmap;
  {
    ComPtr<IWICImagingFactory> imaging;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    const bool decoded =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&imaging))) &&
        SUCCEEDED(imaging->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(kMarkPng), sizeof(kMarkPng))) &&
        SUCCEEDED(imaging->CreateDecoderFromStream(stream.Get(), nullptr,
                                                   WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(imaging->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom));
    if (decoded) {
      canvas->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap);
    }
  }
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return bitmap;
}

// Makes the resources for this canvas; false when nothing can be drawn.
bool prepare(ID2D1DeviceContext* canvas) {
  if (!hud::text::load()) {
    return false;
  }
  if (resources.canvas != canvas) {
    resources = Resources{};
    resources.canvas = canvas;
    resources.mark = decode_mark(canvas);
    if (FAILED(
            canvas->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &resources.brush))) {
      resources.brush.Reset();
    }
  }
  return resources.brush != nullptr;
}

// The line laid out for this text and size (again only when either changed); false when it
// cannot be.
bool lay_out(Line& line, const std::wstring& text, const wchar_t* family, float size,
             DWRITE_FONT_WEIGHT weight, IDWriteFontFallback* fallback, float spacing) {
  if (line.layout && line.text == text && line.size == size) {
    return true;
  }
  line.text = text;
  line.size = size;
  line.layout =
      hud::text::layout(text, family, size, weight, DWRITE_FONT_STYLE_NORMAL, fallback, spacing);
  if (line.layout && FAILED(line.layout->GetMetrics(&line.metrics))) {
    line.layout.Reset();
  }
  return line.layout != nullptr;
}

// A line with the design's shadow under it (0 1px 2px, black at 0.9), its top left at `at`.
void draw_line(ID2D1DeviceContext* canvas, const Line& line, D2D1_POINT_2F at, std::uint32_t rgb,
               float scale) {
  ID2D1SolidColorBrush* brush = resources.brush.Get();
  const float blur = std::max(1.0f, scale);
  const float offsets[][2] = {{-blur, scale}, {blur, scale}, {0, scale - blur}, {0, scale + blur}};
  brush->SetColor(D2D1::ColorF(0, 0, 0, 0.3f));
  for (const auto& offset : offsets) {
    canvas->DrawTextLayout(D2D1::Point2F(at.x + offset[0], at.y + offset[1]), line.layout.Get(),
                           brush);
  }
  brush->SetColor(D2D1::ColorF(0, 0, 0, 0.9f));
  canvas->DrawTextLayout(D2D1::Point2F(at.x, at.y + scale), line.layout.Get(), brush);
  brush->SetColor(color_of(rgb, 1.0f));
  canvas->DrawTextLayout(at, line.layout.Get(), brush);
}

float width_of(const Line& line) {
  return line.metrics.widthIncludingTrailingWhitespace;
}

}  // namespace

void set_in_world(bool in_world) {
  shown.store(in_world);
}

void set_editor(bool open) {
  editor.store(open);
}

void set_server(const std::wstring& name, bool local) {
  std::lock_guard<std::mutex> lock(server_mutex);
  server_name = name;
  server_local = local;
  ping_ms = -1;
}

void set_server_name(const std::string& utf8) {
  std::wstring name;
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  if (length > 0) {
    name.resize(std::size_t(length));
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), name.data(), length);
  }
  std::lock_guard<std::mutex> lock(server_mutex);
  server_name = name;
}

void set_ping(double seconds) {
  std::lock_guard<std::mutex> lock(server_mutex);
  ping_ms = seconds > 0.0 ? int(std::lround(seconds * 1000.0)) : -1;
}

float right_edge() {
  return painted_right.load();
}

float middle() {
  return painted_middle.load();
}

void paint(ID2D1DeviceContext* canvas, float width, float height) {
  if (!shown.load() || !canvas || width <= 0.0f || height <= 0.0f) {
    painted_right.store(0.0f);
    return;
  }
  if (!prepare(canvas)) {
    return;
  }
  const bool in_editor = editor.load();
  // The editor's stage has the window's shape and is 1080 or more high (a narrow window makes it
  // higher, architect/editor_overlay.hpp): the logo is placed on it, at the left end of its menu.
  const float stage_high = in_editor ? architect::overlay::stage_height() : kReferenceHeight;
  const float scale = height / (stage_high > 0.0f ? stage_high : kReferenceHeight);
  // Whole pixels keep the small text sharp.
  const auto pixels = [scale](float design) { return std::max(8.0f, std::round(design * scale)); };
  if (!lay_out(name_line, kName, hud::text::kCinzel, pixels(kNameSize), DWRITE_FONT_WEIGHT_BOLD,
               hud::text::cinzel_fallback(), kNameSpacing * scale)) {
    return;
  }
  // The server line: its name and ping, "localhost" alone for a server on this computer.
  std::wstring server;
  std::wstring ping;
  {
    std::lock_guard<std::mutex> lock(server_mutex);
    if (server_local) {
      server = kLocalhost;
    } else {
      server = server_name;
      if (ping_ms >= 0) {
        ping = std::to_wstring(ping_ms) + L" ms";
      }
    }
  }
  bool has_line = false;
  if (!in_editor) {
    const float line_size = pixels(kLineSize);
    const auto garamond = hud::text::garamond_fallback();
    has_line = !server.empty() || !ping.empty();
    if (!server.empty()) {
      has_line = lay_out(server_line, server, hud::text::kGaramond, line_size,
                         DWRITE_FONT_WEIGHT_NORMAL, garamond, 0.0f);
    }
    if (has_line && !ping.empty()) {
      has_line = lay_out(ping_line, ping, hud::text::kGaramond, line_size,
                         DWRITE_FONT_WEIGHT_NORMAL, garamond, 0.0f) &&
                 lay_out(dot_line, kDot, hud::text::kGaramond, line_size, DWRITE_FONT_WEIGHT_NORMAL,
                         garamond, 0.0f);
    }
  }
  const float side = std::round((in_editor ? kEditorMarkSide : kMarkSide) * scale);
  const float gap = std::round((in_editor ? kEditorGap : kGap) * scale);
  const float line_high = !server.empty() ? server_line.metrics.height : ping_line.metrics.height;
  const float text_high =
      name_line.metrics.height + (has_line ? kLineGap * scale + line_high : 0.0f);
  // The mark and the text centred on each other, the taller one on the bottom margin.
  const float block = std::max(side, text_high);
  float left = std::round((in_editor ? kEditorLeft : kLeft) * scale);
  float top =
      in_editor ? std::round(kEditorTop * scale) : std::round(height - kBottom * scale - block);
  if (resources.mark) {
    const float mark_top = std::round(top + (block - side) / 2.0f);
    canvas->DrawBitmap(resources.mark.Get(),
                       D2D1::RectF(left, mark_top, left + side, mark_top + side), 1.0f,
                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    left += side + gap;
  }
  const float text_top = std::round(top + (block - text_high) / 2.0f);
  draw_line(canvas, name_line, D2D1::Point2F(left, text_top), kNameColor, scale);
  float right = left + width_of(name_line);
  if (has_line) {
    const float line_top = std::round(text_top + name_line.metrics.height + kLineGap * scale);
    float x = left;
    if (!server.empty()) {
      draw_line(canvas, server_line, D2D1::Point2F(x, line_top), kServerColor, scale);
      x += width_of(server_line);
    }
    if (!ping.empty()) {
      if (!server.empty()) {
        draw_line(canvas, dot_line, D2D1::Point2F(x, line_top), kDotColor, scale);
        x += width_of(dot_line);
      }
      draw_line(canvas, ping_line, D2D1::Point2F(x, line_top), kPingColor, scale);
      x += width_of(ping_line);
    }
    right = std::max(right, x);
  }
  painted_middle.store(top + block / 2.0f);
  painted_right.store(in_editor ? 0.0f : right);
}

}  // namespace kcds::runtime::branding::watermark
