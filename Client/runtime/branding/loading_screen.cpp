#include "loading_screen.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_1.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#include "loading_screen_art.hpp"

namespace kcds::runtime::branding::loading_screen {
namespace {

using Microsoft::WRL::ComPtr;

// The owner's design (the design canvas's "Loading" and "Loading-Spec" pages): one design unit
// is one pixel at a 1080-pixel-high window, u = height / 1080; the centre of the window is 0,0.
constexpr float kReferenceHeight = 1080.0f;

// The loops (milliseconds): every one a multiple of the 2200 ms beat, so they stay in step.
constexpr double kBeatMs = 2200.0;
constexpr double kGlowMs = 4400.0;
constexpr double kRingMs = 24000.0;
constexpr double kFadeMs = 500.0;
// The screen ends after this long even without the menu (a start that went another way).
constexpr ULONGLONG kLimitMs = 180000;

D2D1_COLOR_F rgb(unsigned value, float alpha = 1.0f) {
  return D2D1::ColorF(static_cast<float>((value >> 16) & 0xff) / 255.0f,
                      static_cast<float>((value >> 8) & 0xff) / 255.0f,
                      static_cast<float>(value & 0xff) / 255.0f, alpha);
}

std::atomic<ReadyCheck> ready_check{nullptr};
std::atomic<Logger> log_line{nullptr};
// Whether the screen's thread runs: show_again starts it only when it does not.
std::atomic<bool> running{false};
// The status line under the ornament, without its ellipsis.
std::atomic<const wchar_t*> status_text{L"Loading the game"};
// When the screen was shown and when the game became ready (0 until then); the screen's thread
// only.
ULONGLONG started_at = 0;
ULONGLONG ready_at = 0;

// CSS's cubic-bezier timing functions: ease-in-out (0.42, 0, 0.58, 1) and ease-out (0, 0, 0.58,
// 1), solved for the curve's x by bisection.
double bezier(double x1, double x2, double t) {
  if (t <= 0.0) {
    return 0.0;
  }
  if (t >= 1.0) {
    return 1.0;
  }
  auto curve = [](double a, double b, double s) {
    const double inverse = 1.0 - s;
    return 3.0 * inverse * inverse * s * a + 3.0 * inverse * s * s * b + s * s * s;
  };
  double low = 0.0;
  double high = 1.0;
  double s = t;
  for (int i = 0; i < 40; ++i) {
    s = (low + high) / 2.0;
    if (curve(x1, x2, s) < t) {
      low = s;
    } else {
      high = s;
    }
  }
  return curve(0.0, 1.0, s);
}

double ease_in_out(double t) {
  return bezier(0.42, 0.58, t);
}

double ease_out(double t) {
  return bezier(0.0, 0.58, t);
}

// A value going a -> b -> a over one loop (the keyframes 0%, 50%, 100%), eased per half.
double there_and_back(double phase, double a, double b) {
  if (phase < 0.5) {
    return a + (b - a) * ease_in_out(phase * 2.0);
  }
  return b + (a - b) * ease_in_out((phase - 0.5) * 2.0);
}

// SVG path data with absolute M, H, V, L, Q and Z commands (the artwork's only ones) into a
// geometry in the path's own units.
ComPtr<ID2D1PathGeometry> parse_path(ID2D1Factory* factory, const char* data) {
  ComPtr<ID2D1PathGeometry> geometry;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) {
    return nullptr;
  }
  sink->SetFillMode(D2D1_FILL_MODE_WINDING);
  const char* at = data;
  char command = 0;
  bool open = false;
  D2D1_POINT_2F pen = D2D1::Point2F(0, 0);
  auto number = [&at]() {
    char* end = nullptr;
    const float value = std::strtof(at, &end);
    at = end;
    return value;
  };
  auto skip = [&at]() {
    while (*at == ' ' || *at == ',') {
      ++at;
    }
  };
  for (;;) {
    skip();
    if (*at == 0) {
      break;
    }
    if (std::strchr("MHVLQZ", *at) != nullptr) {
      command = *at;
      ++at;
      skip();
    }
    if (command == 'Z') {
      if (open) {
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        open = false;
      }
      command = 0;
      continue;
    }
    if (command == 'M') {
      if (open) {
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
      }
      pen.x = number();
      pen.y = number();
      sink->BeginFigure(pen, D2D1_FIGURE_BEGIN_FILLED);
      open = true;
      command = 'L';
    } else if (command == 'H') {
      pen.x = number();
      sink->AddLine(pen);
    } else if (command == 'V') {
      pen.y = number();
      sink->AddLine(pen);
    } else if (command == 'L') {
      pen.x = number();
      pen.y = number();
      sink->AddLine(pen);
    } else if (command == 'Q') {
      D2D1_QUADRATIC_BEZIER_SEGMENT segment;
      segment.point1.x = number();
      segment.point1.y = number();
      pen.x = number();
      pen.y = number();
      segment.point2 = pen;
      sink->AddQuadraticBezier(segment);
    } else {
      // Not a command of the artwork: stop rather than draw something wrong.
      break;
    }
  }
  if (open) {
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
  }
  if (FAILED(sink->Close())) {
    return nullptr;
  }
  return geometry;
}

// What the screen draws with, for one canvas and one scale.
struct Resources {
  ID2D1DeviceContext* canvas = nullptr;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<ID2D1PathGeometry> letter;
  ComPtr<ID2D1PathGeometry> wordmark;
  ComPtr<IDWriteFactory> text_factory;
  float u = 0.0f;
  ComPtr<IDWriteTextFormat> subtitle;
  ComPtr<IDWriteTextFormat> status;
};

Resources resources;

ComPtr<IDWriteTextFormat> make_format(float size, DWRITE_FONT_WEIGHT weight, float line) {
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(resources.text_factory->CreateTextFormat(
          L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
          L"en-US", &format))) {
    return nullptr;
  }
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line, line * 0.8f);
  return format;
}

bool prepare(ID2D1DeviceContext* canvas, float u) {
  if (resources.canvas != canvas) {
    resources = Resources{};
    resources.canvas = canvas;
    ComPtr<ID2D1Factory> factory;
    canvas->GetFactory(&factory);
    if (FAILED(canvas->CreateSolidColorBrush(rgb(0xF6EDD8), &resources.brush))) {
      resources.brush.Reset();
    }
    resources.letter = parse_path(factory.Get(), art::kLetterS);
    resources.wordmark = parse_path(factory.Get(), art::kWordmark);
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(resources.text_factory.GetAddressOf())))) {
      resources.text_factory.Reset();
    }
  }
  if (!resources.brush || !resources.letter || !resources.wordmark || !resources.text_factory) {
    return false;
  }
  if (resources.u != u) {
    resources.subtitle = make_format(15.0f * u, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.0f * u);
    resources.status = make_format(17.0f * u, DWRITE_FONT_WEIGHT_NORMAL, 24.0f * u);
    resources.u = u;
  }
  return resources.subtitle && resources.status;
}

ComPtr<ID2D1RadialGradientBrush> radial(const D2D1_GRADIENT_STOP* stops, UINT32 count,
                                        D2D1_POINT_2F centre, float radius_x, float radius_y) {
  ComPtr<ID2D1GradientStopCollection> collection;
  ComPtr<ID2D1RadialGradientBrush> brush;
  if (FAILED(resources.canvas->CreateGradientStopCollection(stops, count, &collection)) ||
      FAILED(resources.canvas->CreateRadialGradientBrush(
          D2D1::RadialGradientBrushProperties(centre, D2D1::Point2F(0, 0), radius_x, radius_y),
          collection.Get(), &brush))) {
    return nullptr;
  }
  return brush;
}

ComPtr<ID2D1LinearGradientBrush> linear(const D2D1_GRADIENT_STOP* stops, UINT32 count,
                                        D2D1_POINT_2F from, D2D1_POINT_2F to) {
  ComPtr<ID2D1GradientStopCollection> collection;
  ComPtr<ID2D1LinearGradientBrush> brush;
  if (FAILED(resources.canvas->CreateGradientStopCollection(stops, count, &collection)) ||
      FAILED(resources.canvas->CreateLinearGradientBrush(
          D2D1::LinearGradientBrushProperties(from, to), collection.Get(), &brush))) {
    return nullptr;
  }
  return brush;
}

// Text centred on x, its line box starting at top; letter spacing in ems, split around each
// letter so the line stays centred.
void text(const std::wstring& value, IDWriteTextFormat* format, float x, float top, float size,
          float spacing_em, D2D1_COLOR_F color) {
  const float width = 1280.0f * resources.u;
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(resources.text_factory->CreateTextLayout(value.c_str(),
                                                      static_cast<UINT32>(value.size()), format,
                                                      width, 200.0f * resources.u, &layout))) {
    return;
  }
  ComPtr<IDWriteTextLayout1> spaced;
  if (spacing_em > 0.0f && SUCCEEDED(layout.As(&spaced))) {
    const float half = spacing_em * size / 2.0f;
    spaced->SetCharacterSpacing(half, half, 0.0f,
                                DWRITE_TEXT_RANGE{0, static_cast<UINT32>(value.size())});
  }
  resources.brush->SetColor(color);
  resources.canvas->DrawTextLayout(D2D1::Point2F(x - width / 2, top), layout.Get(),
                                   resources.brush.Get());
}

void paint_background(float width, float height, D2D1_POINT_2F centre) {
  ID2D1DeviceContext* canvas = resources.canvas;
  const float u = resources.u;
  const D2D1_RECT_F all = D2D1::RectF(0, 0, width, height);
  resources.brush->SetColor(rgb(0x100E0B));
  canvas->FillRectangle(all, resources.brush.Get());
  // The light: an ellipse 1400 x 900 (radii) at 50% / 44%.
  const D2D1_GRADIENT_STOP light_stops[] = {
      {0.0f, rgb(0x2A2117)}, {0.45f, rgb(0x18130E)}, {0.75f, rgb(0x100E0B)}, {1.0f, rgb(0x100E0B)}};
  if (auto light = radial(light_stops, 4, D2D1::Point2F(centre.x, height * 0.44f), 1400.0f * u,
                          900.0f * u)) {
    canvas->FillRectangle(all, light.Get());
  }
  // The vignette over the whole window (CSS farthest-corner ellipse).
  const D2D1_GRADIENT_STOP vignette_stops[] = {
      {0.0f, rgb(0x050403, 0.0f)}, {0.55f, rgb(0x050403, 0.0f)}, {1.0f, rgb(0x050403, 0.65f)}};
  if (auto vignette =
          radial(vignette_stops, 3, centre, width / 2 * 1.41421356f, height / 2 * 1.41421356f)) {
    canvas->FillRectangle(all, vignette.Get());
  }
}

// The glow behind the mark: a 520 circle at y -140 whose gradient runs to the box's corner,
// breathing in opacity and size over 4400 ms.
void paint_glow(D2D1_POINT_2F centre, double now_ms) {
  const float u = resources.u;
  const double phase = std::fmod(now_ms, kGlowMs) / kGlowMs;
  const float opacity = static_cast<float>(there_and_back(phase, 0.35, 0.70));
  const float scale = static_cast<float>(there_and_back(phase, 0.96, 1.04));
  const D2D1_POINT_2F middle = D2D1::Point2F(centre.x, centre.y - 140.0f * u);
  const float radius = 260.0f * u * scale;
  const float reach = radius * 1.41421356f;
  const D2D1_GRADIENT_STOP stops[] = {{0.0f, rgb(0xC9A24A, 0.30f * opacity)},
                                      {0.38f, rgb(0xC9A24A, 0.10f * opacity)},
                                      {0.68f, rgb(0xC9A24A, 0.0f)},
                                      {1.0f, rgb(0xC9A24A, 0.0f)}};
  if (auto glow = radial(stops, 4, middle, reach, reach)) {
    resources.canvas->FillEllipse(D2D1::Ellipse(middle, radius, radius), glow.Get());
  }
}

// The ring: 300 at y -140 (its own 512-unit box), turning once in 24 s.
void paint_ring(D2D1_POINT_2F centre, double now_ms) {
  ID2D1DeviceContext* canvas = resources.canvas;
  const float u = resources.u;
  const float scale = 300.0f * u / 512.0f;
  const float angle = static_cast<float>(std::fmod(now_ms, kRingMs) / kRingMs * 360.0);
  D2D1_MATRIX_3X2_F before;
  canvas->GetTransform(&before);
  canvas->SetTransform(D2D1::Matrix3x2F::Translation(-256.0f, -256.0f) *
                       D2D1::Matrix3x2F::Rotation(angle) * D2D1::Matrix3x2F::Scale(scale, scale) *
                       D2D1::Matrix3x2F::Translation(centre.x, centre.y - 140.0f * u) * before);
  const D2D1_POINT_2F middle = D2D1::Point2F(256.0f, 256.0f);
  // Outer dotted circle (dashes of 2 every 12 units) and the inner plain one.
  ComPtr<ID2D1Factory> factory;
  canvas->GetFactory(&factory);
  const float dashes[] = {1.0f, 5.0f};
  ComPtr<ID2D1StrokeStyle> dotted;
  factory->CreateStrokeStyle(
      D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                  D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f),
      dashes, 2, &dotted);
  resources.brush->SetColor(rgb(0xC9A24A, 0.32f));
  canvas->DrawEllipse(D2D1::Ellipse(middle, 240.0f, 240.0f), resources.brush.Get(), 2.0f,
                      dotted.Get());
  resources.brush->SetColor(rgb(0xC9A24A, 0.16f));
  canvas->DrawEllipse(D2D1::Ellipse(middle, 224.0f, 224.0f), resources.brush.Get(), 2.0f);
  // Twelve ticks at the top of the circle, turned by 30 degrees each; every third long and bright.
  for (int tick = 0; tick < 12; ++tick) {
    const bool major = tick % 3 == 0;
    canvas->SetTransform(D2D1::Matrix3x2F::Rotation(30.0f * tick, middle) *
                         D2D1::Matrix3x2F::Translation(-256.0f, -256.0f) *
                         D2D1::Matrix3x2F::Rotation(angle) * D2D1::Matrix3x2F::Scale(scale, scale) *
                         D2D1::Matrix3x2F::Translation(centre.x, centre.y - 140.0f * u) * before);
    resources.brush->SetColor(rgb(0xC9A24A, major ? 0.9f : 0.45f));
    canvas->FillRectangle(D2D1::RectF(254.0f, 14.0f, 258.0f, major ? 28.0f : 22.0f),
                          resources.brush.Get());
  }
  canvas->SetTransform(before);
}

// The mark: 168 at y -140 (its own 256-unit box), with a soft shadow; the corner brackets pulse
// 0.55 -> 1 -> 0.55 every 2200 ms.
void paint_mark(D2D1_POINT_2F centre, double now_ms) {
  ID2D1DeviceContext* canvas = resources.canvas;
  const float u = resources.u;
  const float side = 168.0f * u;
  const float left = centre.x - side / 2;
  const float top = centre.y - 224.0f * u;
  // The shadow (0 10 30 rgba(0,0,0,.55)), as rings of a rounded square fading outwards: 12
  // layers, each a little wider and fainter, add up to a soft edge like the design's blur.
  constexpr int kShadowLayers = 12;
  for (int ring = kShadowLayers; ring >= 1; --ring) {
    const float grow = 24.0f * u * static_cast<float>(ring) / kShadowLayers - 6.0f * u;
    const float corner = side * 52.0f / 256.0f + grow;
    const D2D1_RECT_F place = D2D1::RectF(
        left + side * 8.0f / 256.0f - grow, top + side * 8.0f / 256.0f - grow + 10.0f * u,
        left + side * 248.0f / 256.0f + grow, top + side * 248.0f / 256.0f + grow + 10.0f * u);
    resources.brush->SetColor(rgb(0x000000, 0.45f / kShadowLayers));
    canvas->FillRoundedRectangle(D2D1::RoundedRect(place, corner, corner), resources.brush.Get());
  }
  D2D1_MATRIX_3X2_F before;
  canvas->GetTransform(&before);
  const float scale = side / 256.0f;
  canvas->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                       D2D1::Matrix3x2F::Translation(left, top) * before);
  // Layer 1: the frame.
  const D2D1_GRADIENT_STOP fill_stops[] = {{0.0f, rgb(0x2A2118)}, {1.0f, rgb(0x0F0C08)}};
  if (auto fill = linear(fill_stops, 2, D2D1::Point2F(8, 8), D2D1::Point2F(248, 248))) {
    canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(8, 8, 248, 248), 52, 52),
                                 fill.Get());
  }
  resources.brush->SetColor(rgb(0xC9A45C, 0.5f));
  canvas->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(14, 14, 242, 242), 47, 47),
                               resources.brush.Get(), 4.0f);
  // Layer 2: the brackets (square line ends).
  ComPtr<ID2D1Factory> factory;
  canvas->GetFactory(&factory);
  ComPtr<ID2D1StrokeStyle> square;
  factory->CreateStrokeStyle(
      D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_SQUARE, D2D1_CAP_STYLE_SQUARE,
                                  D2D1_CAP_STYLE_SQUARE, D2D1_LINE_JOIN_MITER),
      nullptr, 0, &square);
  const double phase = std::fmod(now_ms, kBeatMs) / kBeatMs;
  resources.brush->SetColor(rgb(0xE2BD72, static_cast<float>(there_and_back(phase, 0.55, 1.0))));
  const float corners[4][6] = {{52, 92, 52, 52, 92, 52},
                               {204, 92, 204, 52, 164, 52},
                               {52, 164, 52, 204, 92, 204},
                               {204, 164, 204, 204, 164, 204}};
  for (const auto& corner : corners) {
    canvas->DrawLine(D2D1::Point2F(corner[0], corner[1]), D2D1::Point2F(corner[2], corner[3]),
                     resources.brush.Get(), 12.0f, square.Get());
    canvas->DrawLine(D2D1::Point2F(corner[2], corner[3]), D2D1::Point2F(corner[4], corner[5]),
                     resources.brush.Get(), 12.0f, square.Get());
  }
  // Layer 3: the S.
  resources.brush->SetColor(rgb(0xF6EDD8));
  canvas->FillGeometry(resources.letter.Get(), resources.brush.Get());
  canvas->SetTransform(before);
}

// The name: the wordmark's curves, 207 x 47 with its top at y +40.
void paint_wordmark(D2D1_POINT_2F centre) {
  ID2D1DeviceContext* canvas = resources.canvas;
  const float u = resources.u;
  // The box -2.0 -3.1 470.7 106.6 fitted into 207 x 47, centred (SVG's default "meet").
  const float scale = std::min(207.0f / 470.7f, 47.0f / 106.6f) * u;
  const float left = centre.x - 470.7f * scale / 2;
  const float top = centre.y + 40.0f * u + (47.0f * u - 106.6f * scale) / 2;
  D2D1_MATRIX_3X2_F before;
  canvas->GetTransform(&before);
  canvas->SetTransform(D2D1::Matrix3x2F::Translation(2.0f, 3.1f) *
                       D2D1::Matrix3x2F::Scale(scale, scale) *
                       D2D1::Matrix3x2F::Translation(left, top) * before);
  resources.brush->SetColor(rgb(0xF6EDD8));
  canvas->FillGeometry(resources.wordmark.Get(), resources.brush.Get());
  canvas->SetTransform(before);
}

// The ornament at y +150 (420 x 12, clipped): two fading lines, the gold streak running along
// them and the diamond in the middle that lights when the streak crosses it.
void paint_ornament(D2D1_POINT_2F centre, double now_ms) {
  ID2D1DeviceContext* canvas = resources.canvas;
  const float u = resources.u;
  const float left = centre.x - 210.0f * u;
  const float top = centre.y + 150.0f * u;
  const D2D1_RECT_F box = D2D1::RectF(left, top, left + 420.0f * u, top + 12.0f * u);
  canvas->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  const float line_top = top + 5.5f * u;
  const float line_bottom = line_top + std::max(1.0f, 1.0f * u);
  const D2D1_GRADIENT_STOP left_stops[] = {{0.0f, rgb(0xC9A24A, 0.0f)}, {1.0f, rgb(0xC9A24A)}};
  if (auto line =
          linear(left_stops, 2, D2D1::Point2F(left, 0), D2D1::Point2F(left + 194.0f * u, 0))) {
    canvas->FillRectangle(D2D1::RectF(left, line_top, left + 194.0f * u, line_bottom), line.Get());
  }
  const D2D1_GRADIENT_STOP right_stops[] = {{0.0f, rgb(0xC9A24A)}, {1.0f, rgb(0xC9A24A, 0.0f)}};
  if (auto line = linear(right_stops, 2, D2D1::Point2F(box.right - 194.0f * u, 0),
                         D2D1::Point2F(box.right, 0))) {
    canvas->FillRectangle(D2D1::RectF(box.right - 194.0f * u, line_top, box.right, line_bottom),
                          line.Get());
  }
  // The streak: from x -120 to +420 over the first 1700 ms of the beat (ease-in-out), then it
  // waits; it fades in by 220 ms and out between 1430 and 1700 ms.
  const double beat = std::fmod(now_ms, kBeatMs);
  const double run = std::min(1.0, beat / 1700.0);
  const float streak_x = left + static_cast<float>(-120.0 + 540.0 * ease_in_out(run)) * u;
  double streak_opacity = 1.0;
  if (beat < 220.0) {
    streak_opacity = beat / 220.0;
  } else if (beat > 1430.0) {
    streak_opacity = std::max(0.0, 1.0 - (beat - 1430.0) / 270.0);
  }
  if (streak_opacity > 0.0) {
    const float alpha = static_cast<float>(streak_opacity);
    const D2D1_GRADIENT_STOP streak_stops[] = {
        {0.0f, rgb(0xF6E3B4, 0.0f)}, {0.5f, rgb(0xF6E3B4, alpha)}, {1.0f, rgb(0xF6E3B4, 0.0f)}};
    if (auto streak = linear(streak_stops, 3, D2D1::Point2F(streak_x, 0),
                             D2D1::Point2F(streak_x + 120.0f * u, 0))) {
      const D2D1_RECT_F place =
          D2D1::RectF(streak_x, top + 4.0f * u, streak_x + 120.0f * u, top + 8.0f * u);
      canvas->FillRoundedRectangle(D2D1::RoundedRect(place, 2.0f * u, 2.0f * u), streak.Get());
    }
  }
  // The diamond: dark, lit gold at 849 ms (38.6 %), dark again by 1320 ms (60 %), eased.
  double lit = 0.0;
  if (beat < 849.0) {
    lit = ease_in_out(beat / 849.0);
  } else if (beat < 1320.0) {
    lit = 1.0 - ease_in_out((beat - 849.0) / 471.0);
  }
  const D2D1_POINT_2F middle = D2D1::Point2F(left + 208.0f * u, top + 6.0f * u);
  canvas->PopAxisAlignedClip();
  if (lit > 0.0) {
    const D2D1_GRADIENT_STOP glow_stops[] = {{0.0f, rgb(0xE2BD72, 0.55f * static_cast<float>(lit))},
                                             {1.0f, rgb(0xE2BD72, 0.0f)}};
    if (auto glow = radial(glow_stops, 2, middle, 10.0f * u, 10.0f * u)) {
      canvas->FillEllipse(D2D1::Ellipse(middle, 10.0f * u, 10.0f * u), glow.Get());
    }
  }
  D2D1_MATRIX_3X2_F before;
  canvas->GetTransform(&before);
  canvas->SetTransform(D2D1::Matrix3x2F::Rotation(45.0f, middle) * before);
  const D2D1_RECT_F diamond = D2D1::RectF(middle.x - 4.0f * u, middle.y - 4.0f * u,
                                          middle.x + 4.0f * u, middle.y + 4.0f * u);
  const D2D1_COLOR_F dark = rgb(0x100E0B);
  const D2D1_COLOR_F gold = rgb(0xE2BD72);
  const float mix = static_cast<float>(lit);
  resources.brush->SetColor(D2D1::ColorF(dark.r + (gold.r - dark.r) * mix,
                                         dark.g + (gold.g - dark.g) * mix,
                                         dark.b + (gold.b - dark.b) * mix, 1.0f));
  canvas->FillRectangle(diamond, resources.brush.Get());
  resources.brush->SetColor(rgb(0xC9A24A));
  canvas->DrawRectangle(diamond, resources.brush.Get(), std::max(1.0f, 1.0f * u));
  canvas->SetTransform(before);
}

// What the screen shows at `now`: its opacity, or a negative value once it is over.
float step(ULONGLONG now) {
  const ReadyCheck ready = ready_check.load();
  if (ready_at == 0 && ((ready && ready()) || now - started_at > kLimitMs)) {
    ready_at = now;
  }
  if (ready_at == 0) {
    return 1.0f;
  }
  const double gone = static_cast<double>(now - ready_at);
  if (gone >= kFadeMs) {
    return -1.0f;
  }
  return static_cast<float>(1.0 - ease_out(gone / kFadeMs));
}

// Draws the whole screen at full opacity (the window fades as a whole).
void draw(ID2D1DeviceContext* canvas, float width, float height, double now_ms) {
  const float u = height / kReferenceHeight;
  if (!prepare(canvas, u)) {
    return;
  }
  const D2D1_POINT_2F centre = D2D1::Point2F(width / 2, height / 2);
  paint_background(width, height, centre);
  paint_glow(centre, now_ms);
  paint_ring(centre, now_ms);
  paint_mark(centre, now_ms);
  paint_wordmark(centre);
  std::wstring subtitle = L"KINGDOM COME: DELIVERANCE II ";
  subtitle += static_cast<wchar_t>(0x00B7);
  subtitle += L" SYNCED";
  text(subtitle, resources.subtitle.Get(), centre.x, centre.y + 110.0f * u, 15.0f * u, 0.32f,
       rgb(0xC9A24A));
  paint_ornament(centre, now_ms);
  std::wstring status = status_text.load();
  status += static_cast<wchar_t>(0x2026);
  text(status, resources.status.Get(), centre.x, centre.y + 178.0f * u, 17.0f * u, 0.02f,
       rgb(0xB3A88F));
}

struct WindowSearch {
  DWORD process = 0;
  HWND found = nullptr;
};

BOOL CALLBACK check_window(HWND window, LPARAM data) {
  auto* search = reinterpret_cast<WindowSearch*>(data);
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  wchar_t name[32] = {};
  GetClassNameW(window, name, 32);
  if (process == search->process && IsWindowVisible(window) &&
      std::wcscmp(name, L"CryENGINE") == 0) {
    search->found = window;
    return FALSE;
  }
  return TRUE;
}

// The game's own window (class CryENGINE) of this process, once it is shown; null before.
HWND find_game_window() {
  WindowSearch search;
  search.process = GetCurrentProcessId();
  EnumWindows(check_window, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

// The game window's client area on the screen.
RECT client_on_screen(HWND game) {
  RECT client = {};
  GetClientRect(game, &client);
  POINT corner = {0, 0};
  ClientToScreen(game, &corner);
  OffsetRect(&client, corner.x, corner.y);
  return client;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_MOUSEACTIVATE) {
    return MA_NOACTIVATE;
  }
  if (message == WM_SETCURSOR) {
    // The game's own cursor, its window class's (the game window): the Windows arrow
    // blinked in over the game's cursor whenever the pointer crossed the screen.
    // The arrow only before the game has one.
    const HWND game = GetWindow(window, GW_OWNER);
    auto cursor = game ? reinterpret_cast<HCURSOR>(GetClassLongPtrW(game, GCLP_HCURSOR)) : nullptr;
    SetCursor(cursor ? cursor : LoadCursor(nullptr, IDC_ARROW));
    return TRUE;
  }
  if (message == WM_ERASEBKGND) {
    return 1;
  }
  return DefWindowProcW(window, message, w, l);
}

void log_message(const char* text) {
  if (const Logger log = log_line.load()) {
    log("%s", text);
  }
}

// The screen: waits for the game window, covers its client area with our window and draws about
// 60 times a second until the game is ready and the fade is over.
DWORD run_screen() {
  const ULONGLONG waited_from = GetTickCount64();
  HWND game = nullptr;
  while (!game) {
    const ReadyCheck ready = ready_check.load();
    if ((ready && ready()) || GetTickCount64() - waited_from > kLimitMs) {
      log_message("LOADING screen skipped: the game was ready before its window");
      return 0;
    }
    game = find_game_window();
    if (!game) {
      Sleep(15);
    }
  }
  WNDCLASSW type = {};
  type.lpfnWndProc = window_proc;
  type.hInstance = GetModuleHandleW(nullptr);
  type.lpszClassName = L"KCDS.LoadingScreen";
  RegisterClassW(&type);
  RECT place = client_on_screen(game);
  // Owned by the game window: it stays above it, never takes the focus and goes with it.
  HWND window =
      CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED, type.lpszClassName,
                      L"KCDS", WS_POPUP, place.left, place.top, place.right - place.left,
                      place.bottom - place.top, game, nullptr, type.hInstance, nullptr);
  if (!window) {
    log_message("LOADING screen window could not be made");
    return 0;
  }
  SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
  ShowWindow(window, SW_SHOWNOACTIVATE);

  ComPtr<ID2D1Factory> factory;
  ComPtr<ID2D1HwndRenderTarget> target;
  ComPtr<ID2D1DeviceContext> canvas;
  const bool drawable =
      SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) &&
      SUCCEEDED(factory->CreateHwndRenderTarget(
          D2D1::RenderTargetProperties(),
          D2D1::HwndRenderTargetProperties(
              window, D2D1::SizeU(place.right - place.left, place.bottom - place.top)),
          &target)) &&
      SUCCEEDED(target.As(&canvas));
  if (!drawable) {
    log_message("LOADING screen could not draw; it is left out");
    DestroyWindow(window);
    return 0;
  }
  target->SetDpi(96.0f, 96.0f);
  started_at = GetTickCount64();
  if (const Logger log = log_line.load()) {
    log("LOADING screen shown %ldx%ld", place.right - place.left, place.bottom - place.top);
  }
  BYTE shown_opacity = 255;
  for (;;) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    const ULONGLONG now = GetTickCount64();
    const float opacity = step(now);
    if (opacity < 0.0f || !IsWindow(game)) {
      break;
    }
    // Follows the game window: moved, resized or switched between windowed and full screen.
    const RECT current = client_on_screen(game);
    const LONG width = current.right - current.left;
    const LONG height = current.bottom - current.top;
    if (std::memcmp(&current, &place, sizeof(RECT)) != 0 && width > 0 && height > 0) {
      place = current;
      SetWindowPos(window, nullptr, place.left, place.top, width, height,
                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
      target->Resize(D2D1::SizeU(width, height));
    }
    const BYTE wanted_opacity = static_cast<BYTE>(opacity * 255.0f);
    if (wanted_opacity != shown_opacity) {
      shown_opacity = wanted_opacity;
      SetLayeredWindowAttributes(window, 0, shown_opacity, LWA_ALPHA);
    }
    const D2D1_SIZE_F size = target->GetSize();
    target->BeginDraw();
    draw(canvas.Get(), size.width, size.height, static_cast<double>(now - started_at));
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
      log_message("LOADING screen lost its drawing target; it ends");
      break;
    }
    Sleep(16);
  }
  if (const Logger log = log_line.load()) {
    log("LOADING screen ended after %llu ms",
        static_cast<unsigned long long>(GetTickCount64() - started_at));
  }
  resources = Resources{};
  canvas.Reset();
  target.Reset();
  DestroyWindow(window);
  return 0;
}

DWORD WINAPI screen_thread(LPVOID) {
  ready_at = 0;
  const DWORD done = run_screen();
  running.store(false);
  return done;
}

void start_thread() {
  running.store(true);
  const HANDLE thread = CreateThread(nullptr, 0, screen_thread, nullptr, 0, nullptr);
  if (thread) {
    CloseHandle(thread);
  } else {
    running.store(false);
  }
}

}  // namespace

void install(ReadyCheck ready, Logger logger) {
  ready_check.store(ready);
  log_line.store(logger);
  start_thread();
}

void show_again(ReadyCheck ready, const wchar_t* status) {
  if (running.load()) {
    return;
  }
  ready_check.store(ready);
  status_text.store(status);
  start_thread();
}

void set_status(const wchar_t* status) {
  status_text.store(status);
}

bool write_preview(const wchar_t* path, unsigned width, unsigned height, double ms) {
  ComPtr<IWICImagingFactory> imaging;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1Factory> factory;
  ComPtr<ID2D1RenderTarget> target;
  ComPtr<ID2D1DeviceContext> canvas;
  const bool ready =
      SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&imaging))) &&
      SUCCEEDED(imaging->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                      WICBitmapCacheOnLoad, &bitmap)) &&
      SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) &&
      SUCCEEDED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(),
                                                     &target)) &&
      SUCCEEDED(target.As(&canvas));
  if (!ready) {
    return false;
  }
  target->SetDpi(96.0f, 96.0f);
  target->BeginDraw();
  draw(canvas.Get(), static_cast<float>(width), static_cast<float>(height), ms);
  const bool drawn = SUCCEEDED(target->EndDraw());
  resources = Resources{};
  canvas.Reset();
  target.Reset();
  if (!drawn) {
    return false;
  }
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  return SUCCEEDED(imaging->CreateStream(&stream)) &&
         SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
         SUCCEEDED(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
         SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height)) &&
         SUCCEEDED(frame->SetPixelFormat(&format)) &&
         SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
}

}  // namespace kcds::runtime::branding::loading_screen
