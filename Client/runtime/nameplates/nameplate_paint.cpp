#include "nameplate_paint.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../hud/hud_text.hpp"

namespace kcds::runtime::nameplates {
namespace {

using Microsoft::WRL::ComPtr;

// The design's sizes at a 1080-pixel-high frame.
constexpr float kReferenceHeight = 1080.0f;
// The whole plate a quarter bigger than the design's sizes (the design's
// own scene draws the nearest plate at that size too).
constexpr float kPlateScale = 1.25f;
constexpr float kNameSize = 19.0f;
constexpr float kNameSpacing = 1.2f;
// The title larger than the design's 14 px.
constexpr float kTitleSize = 18.0f;
constexpr float kLabelSize = 16.0f;
constexpr float kGap = 4.0f;
// Between the top of the head and the plate.
constexpr float kHeadGap = 8.0f;
// The least room between a plate moved down close up and the top of the screen or the compass
// (share of the screen height).
constexpr float kTopMargin = 0.01f;
constexpr float kBarWidth = 132.0f;
constexpr float kBarHeight = 6.0f;
constexpr float kCapSide = 7.0f;
constexpr float kIconSide = 16.0f;
constexpr float kIconGap = 7.0f;
// A far plate: faded, the name in a paler cream.
constexpr float kFarOpacity = 0.75f;
// Under this share the bar is critical.
constexpr float kCritical = 0.25f;
constexpr float kPi = 3.14159265f;
// The nearest plates of a crowd in the full design; the others without halo, caps and rim.
constexpr std::size_t kFullPlates = 32;
// How many of the nearest bodies in view have a plate: a crowd's plates otherwise make one solid
// band. One that has a plate keeps it up to the larger number, so plates do not flicker as bodies
// change places.
constexpr std::size_t kPlatesShown = 48;
constexpr std::size_t kPlatesKept = 72;
// Text sizes are laid out in half pixels, so a moving plate reuses its layout.
constexpr float kLayoutSizesPerPixel = 2.0f;
// Layouts kept before the cache starts again.
constexpr std::size_t kMostLayouts = 8192;

// The design's colours, 0xRRGGBB.
constexpr std::uint32_t kCream = 0xF6EDD8;
constexpr std::uint32_t kFarCream = 0xE2D6BB;
constexpr std::uint32_t kGold = 0xE4C27A;
constexpr std::uint32_t kCapGold = 0xC9A45C;
constexpr std::uint32_t kRimGold = 0xC9A45C;
constexpr std::uint32_t kTrack = 0x0A0805;
constexpr std::uint32_t kRed = 0xC4493A;
constexpr std::uint32_t kDeepRed = 0x8E2418;
constexpr std::uint32_t kBrightRed = 0xE0624D;
constexpr std::uint32_t kCriticalDeepRed = 0xB8301F;
// The tag colour the server leaves as it is: the design's cream.
constexpr std::uint32_t kDefaultTagColor = 0xFFFFFFFFu;

// Where the game's HUD sits, in screen heights: x from the centre, y from the top (measured on
// the kit's 960 x 540 screenshots with the default HUD size, with a margin).
struct HudArea {
  float left;
  float right;
  float top;
  float bottom;
};
constexpr HudArea kHudAreas[] = {
    {-0.17f, 0.17f, 0.0f, 0.09f},  // the compass
    {-0.19f, 0.19f, 0.89f, 1.0f},  // the health and stamina bars
};

using hud::text::kCinzel;
using hud::text::kGaramond;

std::mutex plates_mutex;
std::vector<Plate> latest;

// What the plates draw with on one canvas: the brushes and the icons (the fonts: hud/hud_text).
struct Resources {
  ID2D1DeviceContext* canvas = nullptr;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<ID2D1GradientStopCollection> fill_stops;
  ComPtr<ID2D1GradientStopCollection> critical_stops;
  ComPtr<ID2D1LinearGradientBrush> fill_gradient;
  ComPtr<ID2D1LinearGradientBrush> critical_gradient;
  ComPtr<ID2D1PathGeometry> horn;
  ComPtr<ID2D1PathGeometry> mic;
};
Resources resources;

D2D1_COLOR_F color_of(std::uint32_t rgb, float alpha) {
  return D2D1::ColorF(float((rgb >> 16) & 0xFF) / 255.0f, float((rgb >> 8) & 0xFF) / 255.0f,
                      float(rgb & 0xFF) / 255.0f, alpha);
}

struct CachedLayout {
  ComPtr<IDWriteTextLayout> layout;
  DWRITE_TEXT_METRICS metrics{};
};
std::unordered_map<std::wstring, CachedLayout> layouts;

// A line's layout and metrics, made once for its text and size; null when it cannot be laid out.
const CachedLayout* cached_layout(const std::wstring& text, const wchar_t* family, float size,
                                  DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
                                  IDWriteFontFallback* fallback, float spacing) {
  const int steps = int(std::lround(size * kLayoutSizesPerPixel));
  std::wstring key = text;
  key += L'\x1';
  key += family;
  key += L'\x1';
  key += std::to_wstring(steps) + L'/' + std::to_wstring(int(weight)) + L'/' +
         std::to_wstring(int(style));
  const auto found = layouts.find(key);
  if (found != layouts.end()) {
    return found->second.layout ? &found->second : nullptr;
  }
  if (layouts.size() >= kMostLayouts) {
    layouts.clear();
  }
  CachedLayout made;
  made.layout = hud::text::layout(text, family, float(steps) / kLayoutSizesPerPixel, weight, style,
                                  fallback, spacing);
  if (made.layout && FAILED(made.layout->GetMetrics(&made.metrics))) {
    made.layout.Reset();
  }
  const auto kept = layouts.emplace(std::move(key), std::move(made)).first;
  return kept->second.layout ? &kept->second : nullptr;
}

// The icon "speaking", in the design's 24-unit box: a horn's mouth and two sound arcs.
ComPtr<ID2D1PathGeometry> make_horn(ID2D1DeviceContext* canvas) {
  ComPtr<ID2D1Factory> factory;
  canvas->GetFactory(&factory);
  ComPtr<ID2D1PathGeometry> horn;
  ComPtr<ID2D1GeometrySink> sink;
  if (!factory || FAILED(factory->CreatePathGeometry(&horn)) || FAILED(horn->Open(&sink))) {
    return nullptr;
  }
  sink->BeginFigure(D2D1::Point2F(4, 10), D2D1_FIGURE_BEGIN_HOLLOW);
  const D2D1_POINT_2F mouth[] = {{4, 14}, {7, 14}, {12, 18}, {12, 6}, {7, 10}};
  sink->AddLines(mouth, UINT32(std::size(mouth)));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->BeginFigure(D2D1::Point2F(16, 9), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(16, 15), D2D1::SizeF(4, 4), 0.0f,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->BeginFigure(D2D1::Point2F(19, 6), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(19, 18), D2D1::SizeF(8, 8), 0.0f,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  return SUCCEEDED(sink->Close()) ? horn : nullptr;
}

// The icon "talking on voice chat", in the same box: a microphone on its stand.
ComPtr<ID2D1PathGeometry> make_mic(ID2D1DeviceContext* canvas) {
  ComPtr<ID2D1Factory> factory;
  canvas->GetFactory(&factory);
  ComPtr<ID2D1PathGeometry> mic;
  ComPtr<ID2D1GeometrySink> sink;
  if (!factory || FAILED(factory->CreatePathGeometry(&mic)) || FAILED(mic->Open(&sink))) {
    return nullptr;
  }
  sink->BeginFigure(D2D1::Point2F(9, 6), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(15, 6), D2D1::SizeF(3, 3), 0.0f,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
  sink->AddLine(D2D1::Point2F(15, 12));
  sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(9, 12), D2D1::SizeF(3, 3), 0.0f,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->BeginFigure(D2D1::Point2F(5, 11), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(19, 11), D2D1::SizeF(7, 7), 0.0f,
                                D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->BeginFigure(D2D1::Point2F(12, 18), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddLine(D2D1::Point2F(12, 22));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->BeginFigure(D2D1::Point2F(8, 22), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddLine(D2D1::Point2F(16, 22));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  return SUCCEEDED(sink->Close()) ? mic : nullptr;
}

ComPtr<ID2D1GradientStopCollection> stops_of(ID2D1DeviceContext* canvas, std::uint32_t top,
                                             std::uint32_t bottom) {
  const D2D1_GRADIENT_STOP stops[] = {{0.0f, color_of(top, 1.0f)}, {1.0f, color_of(bottom, 1.0f)}};
  ComPtr<ID2D1GradientStopCollection> collection;
  canvas->CreateGradientStopCollection(stops, 2, &collection);
  return collection;
}

bool prepare(ID2D1DeviceContext* canvas) {
  if (!hud::text::load()) {
    return false;
  }
  if (resources.canvas != canvas) {
    resources = Resources{};
    resources.canvas = canvas;
    canvas->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &resources.brush);
    resources.fill_stops = stops_of(canvas, kRed, kDeepRed);
    resources.critical_stops = stops_of(canvas, kBrightRed, kCriticalDeepRed);
    const auto gradient_of = [canvas](ID2D1GradientStopCollection* stops,
                                      ComPtr<ID2D1LinearGradientBrush>& gradient) {
      if (stops) {
        canvas->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)), stops,
            &gradient);
      }
    };
    gradient_of(resources.fill_stops.Get(), resources.fill_gradient);
    gradient_of(resources.critical_stops.Get(), resources.critical_gradient);
    resources.horn = make_horn(canvas);
    resources.mic = make_mic(canvas);
  }
  return resources.brush != nullptr;
}

// Whether a box on the screen (pixels) touches the HUD.
bool on_hud(const D2D1_RECT_F& box, float width, float height) {
  const float left = (box.left - width / 2.0f) / height;
  const float right = (box.right - width / 2.0f) / height;
  const float top = box.top / height;
  const float bottom = box.bottom / height;
  for (const HudArea& area : kHudAreas) {
    if (right > area.left && left < area.right && bottom > area.top && top < area.bottom) {
      return true;
    }
  }
  return false;
}

// A text with the design's dark shadow under it, its top-left corner at `at`.
void draw_text(ID2D1DeviceContext* canvas, IDWriteTextLayout* layout, D2D1_POINT_2F at,
               D2D1_COLOR_F color, float scale, bool simple) {
  ID2D1SolidColorBrush* brush = resources.brush.Get();
  // A soft halo of offsets, then the drop shadow one pixel down.
  if (!simple) {
    const float halo = std::max(1.0f, scale);
    const float offsets[][2] = {{-halo, 0}, {halo, 0}, {0, -halo}, {0, halo}};
    brush->SetColor(D2D1::ColorF(0, 0, 0, 0.45f * color.a));
    for (const auto& offset : offsets) {
      canvas->DrawTextLayout(D2D1::Point2F(at.x + offset[0], at.y + offset[1]), layout, brush);
    }
  }
  brush->SetColor(D2D1::ColorF(0, 0, 0, 0.9f * color.a));
  canvas->DrawTextLayout(D2D1::Point2F(at.x, at.y + scale), layout, brush);
  brush->SetColor(color);
  canvas->DrawTextLayout(at, layout, brush);
}

// A diamond cap centred at (x, y).
void draw_cap(ID2D1DeviceContext* canvas, float x, float y, float side, D2D1_COLOR_F color,
              float opacity) {
  D2D1_MATRIX_3X2_F previous;
  canvas->GetTransform(&previous);
  canvas->SetTransform(D2D1::Matrix3x2F::Rotation(45.0f, D2D1::Point2F(x, y)) * previous);
  const D2D1_RECT_F square = D2D1::RectF(x - side / 2, y - side / 2, x + side / 2, y + side / 2);
  ID2D1SolidColorBrush* brush = resources.brush.Get();
  brush->SetColor(D2D1::ColorF(0, 0, 0, 0.7f * opacity));
  canvas->DrawRectangle(square, brush, 1.0f);
  brush->SetColor(color);
  canvas->FillRectangle(square, brush);
  canvas->SetTransform(previous);
}

// The health bar centred across `center`, its top at `top`.
void draw_bar(ID2D1DeviceContext* canvas, float center, float top, float share, float scale,
              float opacity, double now, bool simple) {
  const float width = std::round(kBarWidth * scale);
  const float height = std::max(3.0f, std::round(kBarHeight * scale));
  const float cap = std::max(4.0f, std::round(kCapSide * scale));
  const bool critical = share < kCritical;
  const D2D1_RECT_F track = D2D1::RectF(center - width / 2, top, center + width / 2, top + height);
  ID2D1SolidColorBrush* brush = resources.brush.Get();
  if (critical) {
    // The glow pulses once a second.
    const float pulse = 0.5f + 0.5f * std::sin(float(now) * 2.0f * kPi);
    for (int ring = 1; ring <= 3; ++ring) {
      const float grow = float(ring) * 1.5f * scale;
      brush->SetColor(color_of(kBrightRed, (0.22f - 0.06f * float(ring)) * pulse * opacity));
      canvas->DrawRectangle(
          D2D1::RectF(track.left - grow, track.top - grow, track.right + grow, track.bottom + grow),
          brush, 1.5f * scale);
    }
  } else if (!simple) {
    brush->SetColor(D2D1::ColorF(0, 0, 0, 0.6f * opacity));
    canvas->FillRectangle(
        D2D1::RectF(track.left, track.top + scale, track.right, track.bottom + 2.0f * scale),
        brush);
  }
  brush->SetColor(color_of(kTrack, 0.72f * opacity));
  canvas->FillRectangle(track, brush);
  if (share > 0.0f) {
    const D2D1_RECT_F fill = D2D1::RectF(track.left, track.top,
                                         track.left + width * std::min(share, 1.0f), track.bottom);
    ID2D1LinearGradientBrush* gradient =
        critical ? resources.critical_gradient.Get() : resources.fill_gradient.Get();
    if (gradient) {
      gradient->SetStartPoint(D2D1::Point2F(0, track.top));
      gradient->SetEndPoint(D2D1::Point2F(0, track.bottom));
      gradient->SetOpacity(opacity);
      canvas->FillRectangle(fill, gradient);
    }
    if (simple) {
      return;
    }
    // The fill's lit top edge.
    brush->SetColor(D2D1::ColorF(1.0f, 0.86f, 0.78f, 0.35f * opacity));
    canvas->FillRectangle(D2D1::RectF(fill.left, fill.top, fill.right, fill.top + 1.0f), brush);
  }
  brush->SetColor(critical ? color_of(kBrightRed, 0.85f * opacity)
                           : color_of(kRimGold, 0.7f * opacity));
  canvas->DrawRectangle(
      D2D1::RectF(track.left - 0.5f, track.top - 0.5f, track.right + 0.5f, track.bottom + 0.5f),
      brush, 1.0f);
  const D2D1_COLOR_F cap_color =
      critical ? color_of(kBrightRed, opacity) : color_of(kCapGold, opacity);
  const float middle = (track.top + track.bottom) / 2;
  draw_cap(canvas, track.left, middle, cap, cap_color, opacity);
  draw_cap(canvas, track.right, middle, cap, cap_color, opacity);
}

void draw_icon(ID2D1DeviceContext* canvas, ID2D1PathGeometry* icon, float left, float middle,
               float side, float opacity) {
  if (!icon) {
    return;
  }
  const float unit = side / 24.0f;
  D2D1_MATRIX_3X2_F previous;
  canvas->GetTransform(&previous);
  canvas->SetTransform(D2D1::Matrix3x2F::Scale(unit, unit) *
                       D2D1::Matrix3x2F::Translation(left, middle - side / 2) * previous);
  ID2D1SolidColorBrush* brush = resources.brush.Get();
  brush->SetColor(D2D1::ColorF(0, 0, 0, 0.6f * opacity));
  canvas->DrawGeometry(icon, brush, 3.5f);
  brush->SetColor(color_of(kGold, opacity));
  canvas->DrawGeometry(icon, brush, 2.0f);
  canvas->SetTransform(previous);
}

// The height of a line of text as paint_plate lays it out (0 when it cannot be laid out).
float line_height(const std::wstring& text, const wchar_t* family, float size,
                  DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, IDWriteFontFallback* fallback,
                  float spacing) {
  const CachedLayout* line = cached_layout(text, family, size, weight, style, fallback, spacing);
  return line ? line->metrics.height : 0.0f;
}

// The whole plate's height on the screen: the bar, the name, the title and the label with their
// gaps, as paint_plate stacks them.
float plate_height(const Plate& plate, float scale) {
  const float gap = kGap * scale;
  float total = 0.0f;
  if (plate.health >= 0.0f && !plate.distant) {
    const float cap = std::max(4.0f, std::round(kCapSide * scale));
    const float bar = std::max(3.0f, std::round(kBarHeight * scale));
    total += cap + bar + gap;
  }
  if (!plate.name.empty()) {
    total +=
        line_height(plate.name, kCinzel, kNameSize * scale, DWRITE_FONT_WEIGHT_BOLD,
                    DWRITE_FONT_STYLE_NORMAL, hud::text::cinzel_fallback(), kNameSpacing * scale) +
        gap;
  }
  if (!plate.title.empty() && !plate.distant) {
    const float size = plate.title_size > 0.0f ? plate.title_size : kTitleSize;
    total += line_height(plate.title, kGaramond, size * scale, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_ITALIC, hud::text::garamond_fallback(), 0.0f) +
             gap;
  }
  if (!plate.label.empty()) {
    total += line_height(plate.label, kGaramond, kLabelSize * scale, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, hud::text::garamond_fallback(), 0.0f) +
             gap;
  }
  return total;
}

// The highest the plate's top may be at horizontal centre cx: below the top of the screen, and
// below the compass where the plate is under it.
float highest_top(float cx, float half_width, float width, float height) {
  float highest = kTopMargin * height;
  for (const HudArea& area : kHudAreas) {
    if (area.top > 0.0f) {
      continue;
    }
    const float left = (cx - half_width - width / 2.0f) / height;
    const float right = (cx + half_width - width / 2.0f) / height;
    if (right > area.left && left < area.right) {
      highest = std::max(highest, (area.bottom + kTopMargin) * height);
    }
  }
  return highest;
}

void paint_plate(ID2D1DeviceContext* canvas, const Plate& plate, float width, float height,
                 double now, bool simple) {
  // Never larger than the full size, however near or far (plate.size is at most 1).
  const float scale = height / kReferenceHeight * kPlateScale * std::min(plate.size, 1.0f);
  const float opacity = (plate.distant ? kFarOpacity : 1.0f) * plate.opacity;
  // On whole pixels: text between pixels shimmers as the plate moves by fractions of one.
  const float cx = std::round(width / 2.0f + plate.x * height);
  const float gap = kGap * scale;
  // The plate stands on its point: the bar at the bottom, the name, the title and the label
  // above, each a fixed size on the screen, so it keeps its height over the head at any distance.
  float bottom = std::round(plate.y * height - kHeadGap * scale);
  // Close up the head reaches the top of the screen and a plate over it would be above the screen
  // or under the compass (the bar must stay on the head): it moves down until
  // it fits, at most by its own height, so it then sits on the top of the head.
  const float stacked = plate_height(plate, scale);
  const float highest = highest_top(cx, kBarWidth * scale / 2 + kCapSide * scale, width, height);
  if (bottom - stacked < highest) {
    bottom += std::min(highest - (bottom - stacked), stacked + kHeadGap * scale);
    bottom = std::round(bottom);
  }
  if (plate.health >= 0.0f && !plate.distant) {
    const float cap = std::max(4.0f, std::round(kCapSide * scale));
    const float bar = std::max(3.0f, std::round(kBarHeight * scale));
    // The caps reach half their side below the track.
    const float top = bottom - cap / 2 - bar;
    const float half = kBarWidth * scale / 2 + cap;
    const D2D1_RECT_F box = D2D1::RectF(cx - half, top - cap / 2, cx + half, bottom);
    if (!on_hud(box, width, height)) {
      draw_bar(canvas, cx, top, plate.health, scale, opacity, now, simple);
    }
    bottom = top - cap / 2 - gap;
  }
  if (!plate.name.empty()) {
    const CachedLayout* name =
        cached_layout(plate.name, kCinzel, kNameSize * scale, DWRITE_FONT_WEIGHT_BOLD,
                      DWRITE_FONT_STYLE_NORMAL, hud::text::cinzel_fallback(), kNameSpacing * scale);
    if (name) {
      const DWRITE_TEXT_METRICS& metrics = name->metrics;
      const float top = std::round(bottom - metrics.height);
      const bool has_icon = plate.speaking || plate.voice_talking;
      const float icon = has_icon ? (kIconSide + kIconGap) * scale : 0.0f;
      const float left = std::round(cx - (metrics.widthIncludingTrailingWhitespace + icon) / 2);
      const D2D1_RECT_F box =
          D2D1::RectF(left, top, left + icon + metrics.widthIncludingTrailingWhitespace, bottom);
      if (!on_hud(box, width, height)) {
        std::uint32_t rgb = plate.distant ? kFarCream : kCream;
        float alpha = opacity;
        if (plate.color != 0 && plate.color != kDefaultTagColor) {
          rgb = plate.color & 0xFFFFFF;
          alpha *= float(plate.color >> 24) / 255.0f;
        }
        if (plate.voice_talking) {
          draw_icon(canvas, resources.mic.Get(), left, (top + bottom) / 2, kIconSide * scale,
                    opacity);
        } else if (plate.speaking) {
          draw_icon(canvas, resources.horn.Get(), left, (top + bottom) / 2, kIconSide * scale,
                    opacity);
        }
        draw_text(canvas, name->layout.Get(), D2D1::Point2F(left + icon, top), color_of(rgb, alpha),
                  scale, simple);
      }
      bottom = top - gap;
    }
  }
  // The title above the name, the label above both.
  const auto line_above = [&](const std::wstring& text, float size, DWRITE_FONT_STYLE style,
                              D2D1_COLOR_F color) {
    const CachedLayout* line =
        cached_layout(text, kGaramond, size * scale, DWRITE_FONT_WEIGHT_NORMAL, style,
                      hud::text::garamond_fallback(), 0.0f);
    if (!line) {
      return;
    }
    const DWRITE_TEXT_METRICS& metrics = line->metrics;
    const float left = std::round(cx - metrics.width / 2);
    const float top = std::round(bottom - metrics.height);
    const D2D1_RECT_F box = D2D1::RectF(left, top, left + metrics.width, bottom);
    if (!on_hud(box, width, height)) {
      draw_text(canvas, line->layout.Get(), D2D1::Point2F(box.left, box.top), color, scale, simple);
    }
    bottom = box.top - gap;
  };
  if (!plate.title.empty() && !plate.distant) {
    const std::uint32_t argb = plate.title_color != 0 ? plate.title_color : 0xFF000000u | kGold;
    // A larger title grows upward only: the lines stack up from the name.
    const float title_size = plate.title_size > 0.0f ? plate.title_size : kTitleSize;
    line_above(plate.title, title_size, DWRITE_FONT_STYLE_ITALIC,
               color_of(argb & 0xFFFFFF, float(argb >> 24) / 255.0f * opacity));
  }
  if (!plate.label.empty()) {
    const std::uint32_t argb = plate.label_color != 0 ? plate.label_color : 0xFF000000u | kCream;
    line_above(plate.label, kLabelSize, DWRITE_FONT_STYLE_NORMAL,
               color_of(argb & 0xFFFFFF, float(argb >> 24) / 255.0f * opacity));
  }
}

}  // namespace

void publish(std::vector<Plate> plates) {
  std::lock_guard<std::mutex> lock(plates_mutex);
  latest = std::move(plates);
}

void paint(ID2D1DeviceContext* canvas, float width, float height) {
  if (!canvas || width <= 0.0f || height <= 0.0f) {
    return;
  }
  std::vector<Plate> plates;
  {
    std::lock_guard<std::mutex> lock(plates_mutex);
    plates = latest;
  }
  if (plates.empty() || !prepare(canvas)) {
    return;
  }
  // The farthest first, so the near plates stand on top.
  std::sort(plates.begin(), plates.end(),
            [](const Plate& a, const Plate& b) { return a.distance > b.distance; });
  const double now = double(GetTickCount64()) / 1000.0;
  const std::size_t simple_plates = plates.size() > kFullPlates ? plates.size() - kFullPlates : 0;
  // Painted farthest first, so `rank` counts down to the nearest.
  static std::unordered_set<std::uint32_t> had_plate;
  std::unordered_set<std::uint32_t> has_plate;
  for (std::size_t i = 0; i < plates.size(); ++i) {
    const Plate& plate = plates[i];
    const std::size_t rank = plates.size() - 1 - i;
    const bool shown = rank < kPlatesShown ||
                       (rank < kPlatesKept && had_plate.count(plate.id) != 0) ||
                       !plate.label.empty() || plate.speaking || plate.voice_talking;
    if (shown) {
      has_plate.insert(plate.id);
      paint_plate(canvas, plate, width, height, now, i < simple_plates);
    }
  }
  had_plate = std::move(has_plate);
}

}  // namespace kcds::runtime::nameplates
