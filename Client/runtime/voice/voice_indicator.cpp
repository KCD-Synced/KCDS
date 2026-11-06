#include "voice_indicator.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

#include "../branding/watermark.hpp"
#include "../hud/hud_text.hpp"

namespace kcds::runtime::voice::indicator {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265f;
constexpr float kReferenceHeight = 1080.0f;
// The design's sizes at 1080 pixels high: the pill 32 high, round, 4 from its left end to the
// microphone's disc (24) and 12 from the last part to its right end, 9 between the parts; 16 from
// the watermark to a 1 x 26 rule and 16 again to the pill.
constexpr float kPillHeight = 32.0f;
constexpr float kPadLeft = 4.0f;
constexpr float kPadRight = 12.0f;
constexpr float kPartGap = 9.0f;
constexpr float kDisc = 24.0f;
constexpr float kGlyph = 15.0f;
constexpr float kRuleGap = 16.0f;
constexpr float kRuleHeight = 26.0f;
constexpr float kLabelSize = 11.0f;
constexpr float kLabelSpacing = 2.0f;
// The range's pips (1 to 3 lit) and the level bars: 3 wide, 2 apart.
constexpr float kBarWidth = 3.0f;
constexpr float kBarGap = 2.0f;
constexpr float kPipHeights[] = {5.0f, 8.0f, 11.0f};
constexpr int kLevelBars = 4;
constexpr float kLevelLow = 3.0f;
constexpr float kLevelHigh = 14.0f;
// The level bars' own motion (0.9 s, each bar a little behind the last) and the ring's pulse.
constexpr float kLevelPeriod = 0.9f;
constexpr float kLevelDelays[] = {0.0f, 0.3f, 0.55f, 0.15f};
constexpr float kRingPeriod = 1.1f;
// The label's flash when the range changes.
constexpr float kFlashSeconds = 0.6f;
// The push-to-talk key's cap: EB Garamond 700 10 px, 1 + 5 padding.
constexpr float kKeySize = 10.0f;

// One look of the pill (the design's .vchip, .is-talk, .is-shout, .is-mute).
struct Look {
  std::uint32_t border;
  float border_alpha;
  std::uint32_t fill_from;
  float fill_from_alpha;
  float fill_to_alpha;  // the fill's right end is rgba(12, 9, 6, a)
  std::uint32_t text;
  std::uint32_t glow;  // 0: none
  float glow_alpha;
  float glow_blur;
  std::uint32_t disc_fill;
  float disc_fill_alpha;
  std::uint32_t disc_border;
  float disc_border_alpha;
  std::uint32_t glyph;
  std::uint32_t pip_on;
  std::uint32_t level;
  std::uint32_t ring;  // 0: no ring
  float ring_alpha;
  float ring_spread;
};

// Idle: the quiet gold outline.
constexpr Look kIdle = {
    .border = 0xC9A45C,
    .border_alpha = 0.32f,
    .fill_from = 0x0C0906,
    .fill_from_alpha = 0.78f,
    .fill_to_alpha = 0.5f,
    .text = 0xCFC3A8,
    .glow = 0,
    .glow_alpha = 0.0f,
    .glow_blur = 0.0f,
    .disc_fill = 0x000000,
    .disc_fill_alpha = 0.35f,
    .disc_border = 0xC9A45C,
    .disc_border_alpha = 0.45f,
    .glyph = 0xB9AD92,
    .pip_on = 0xC9A45C,
    .level = 0xB6DC7C,
    .ring = 0,
    .ring_alpha = 0.0f,
    .ring_spread = 0.0f,
};
// Speaking: the disc fills green, its ring pulses.
constexpr Look kTalk = {
    .border = 0x9CBC6C,
    .border_alpha = 0.75f,
    .fill_from = 0x1C2810,
    .fill_from_alpha = 0.85f,
    .fill_to_alpha = 0.55f,
    .text = 0xDCEFC0,
    .glow = 0x8CBE5A,
    .glow_alpha = 0.25f,
    .glow_blur = 14.0f,
    .disc_fill = 0x9CBC6C,
    .disc_fill_alpha = 1.0f,
    .disc_border = 0xD6ECB4,
    .disc_border_alpha = 1.0f,
    .glyph = 0x13200A,
    .pip_on = 0x9CBC6C,
    .level = 0xB6DC7C,
    .ring = 0x9CBC6C,
    .ring_alpha = 0.65f,
    .ring_spread = 9.0f,
};
// Speaking on shout: amber.
constexpr Look kShout = {
    .border = 0xE0AC55,
    .border_alpha = 0.85f,
    .fill_from = 0x30200A,
    .fill_from_alpha = 0.88f,
    .fill_to_alpha = 0.55f,
    .text = 0xF6DFA6,
    .glow = 0xE0AC55,
    .glow_alpha = 0.3f,
    .glow_blur = 16.0f,
    .disc_fill = 0xE0AC55,
    .disc_fill_alpha = 1.0f,
    .disc_border = 0xF6DFA6,
    .disc_border_alpha = 1.0f,
    .glyph = 0x2A1A05,
    .pip_on = 0xE0AC55,
    .level = 0xF0C26A,
    .ring = 0xE0AC55,
    .ring_alpha = 0.7f,
    .ring_spread = 10.0f,
};
// Muted by the server: red, the microphone crossed out.
constexpr Look kMuted = {
    .border = 0xD0705A,
    .border_alpha = 0.55f,
    .fill_from = 0x0C0906,
    .fill_from_alpha = 0.78f,
    .fill_to_alpha = 0.5f,
    .text = 0xE8A493,
    .glow = 0,
    .glow_alpha = 0.0f,
    .glow_blur = 0.0f,
    .disc_fill = 0x50140C,
    .disc_fill_alpha = 0.55f,
    .disc_border = 0xD0705A,
    .disc_border_alpha = 0.7f,
    .glyph = 0xE8907A,
    .pip_on = 0xC9A45C,
    .level = 0xB6DC7C,
    .ring = 0,
    .ring_alpha = 0.0f,
    .ring_spread = 0.0f,
};
constexpr std::uint32_t kPipOff = 0xC9A45C;
constexpr float kPipOffAlpha = 0.22f;
constexpr std::uint32_t kRule = 0xC9A45C;
constexpr float kRuleAlpha = 0.35f;
constexpr std::uint32_t kKeyText = 0xE9DCBC;
constexpr std::uint32_t kKeyBorder = 0xE9DCBC;
constexpr float kKeyBorderAlpha = 0.45f;

const wchar_t* const kTierLabels[] = {L"WHISPER", L"NORMAL", L"SHOUT"};
constexpr wchar_t kMutedLabel[] = L"MUTED";

std::mutex state_mutex;
State latest;

// What the pill draws with on one canvas.
struct Resources {
  ID2D1DeviceContext* canvas = nullptr;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<ID2D1StrokeStyle> round;
  ComPtr<ID2D1PathGeometry> mic;
  ComPtr<ID2D1PathGeometry> muted_mic;
};
Resources resources;

struct Text {
  std::wstring text;
  float size = 0.0f;
  ComPtr<IDWriteTextLayout> layout;
  DWRITE_TEXT_METRICS metrics{};
};
Text label_text;
Text key_text;

// The range shown last and when it changed (the label's flash).
int shown_tier = -1;
double tier_changed_at = -100.0;

D2D1_COLOR_F color_of(std::uint32_t rgb, float alpha) {
  return D2D1::ColorF(float((rgb >> 16) & 0xFF) / 255.0f, float((rgb >> 8) & 0xFF) / 255.0f,
                      float(rgb & 0xFF) / 255.0f, alpha);
}

double now_seconds() {
  return double(GetTickCount64()) / 1000.0;
}

// Windows' "show animations" setting (the design: no motion with reduced motion).
bool animations_on() {
  BOOL on = TRUE;
  if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) {
    return true;
  }
  return on != FALSE;
}

// The design's microphone, in its 24-unit box (stroke 2, round ends): the capsule, the cup under
// it, the stand and the foot.
ComPtr<ID2D1PathGeometry> make_mic(ID2D1Factory* factory) {
  ComPtr<ID2D1PathGeometry> path;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) {
    return nullptr;
  }
  const auto arc = [&](float x, float y, float radius, D2D1_SWEEP_DIRECTION sweep) {
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(radius, radius), 0.0f, sweep,
                                  D2D1_ARC_SIZE_SMALL));
  };
  sink->BeginFigure(D2D1::Point2F(12, 3), D2D1_FIGURE_BEGIN_HOLLOW);
  arc(9, 6, 3, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE);
  sink->AddLine(D2D1::Point2F(9, 12));
  arc(15, 12, 3, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE);
  sink->AddLine(D2D1::Point2F(15, 6));
  arc(12, 3, 3, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE);
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->BeginFigure(D2D1::Point2F(5.5f, 11), D2D1_FIGURE_BEGIN_HOLLOW);
  arc(18.5f, 11, 6.5f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE);
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->BeginFigure(D2D1::Point2F(12, 17.5f), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddLine(D2D1::Point2F(12, 21));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->BeginFigure(D2D1::Point2F(9, 21), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddLine(D2D1::Point2F(15, 21));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  return SUCCEEDED(sink->Close()) ? path : nullptr;
}

// The design's crossed-out microphone, in the same box.
ComPtr<ID2D1PathGeometry> make_muted_mic(ID2D1Factory* factory) {
  ComPtr<ID2D1PathGeometry> path;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) {
    return nullptr;
  }
  const auto arc = [&](float x, float y, float radius, D2D1_SWEEP_DIRECTION sweep) {
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(radius, radius), 0.0f, sweep,
                                  D2D1_ARC_SIZE_SMALL));
  };
  const auto line = [&](float x1, float y1, float x2, float y2) {
    sink->BeginFigure(D2D1::Point2F(x1, y1), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(D2D1::Point2F(x2, y2));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
  };
  sink->BeginFigure(D2D1::Point2F(9, 9), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddLine(D2D1::Point2F(9, 6));
  arc(14.9f, 5.2f, 3, D2D1_SWEEP_DIRECTION_CLOCKWISE);
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->BeginFigure(D2D1::Point2F(15, 10), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddLine(D2D1::Point2F(15, 12));
  arc(10.4f, 14.5f, 3, D2D1_SWEEP_DIRECTION_CLOCKWISE);
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->BeginFigure(D2D1::Point2F(5.5f, 11), D2D1_FIGURE_BEGIN_HOLLOW);
  arc(15.9f, 16.2f, 6.5f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE);
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->BeginFigure(D2D1::Point2F(18.5f, 11), D2D1_FIGURE_BEGIN_HOLLOW);
  arc(17.9f, 13.7f, 6.5f, D2D1_SWEEP_DIRECTION_CLOCKWISE);
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  line(12, 17.5f, 12, 21);
  line(9, 21, 15, 21);
  line(3, 3, 21, 21);
  return SUCCEEDED(sink->Close()) ? path : nullptr;
}

bool prepare(ID2D1DeviceContext* canvas) {
  if (!hud::text::load()) {
    return false;
  }
  if (resources.canvas != canvas) {
    resources = Resources{};
    resources.canvas = canvas;
    canvas->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &resources.brush);
    ComPtr<ID2D1Factory> factory;
    canvas->GetFactory(&factory);
    if (factory) {
      const D2D1_STROKE_STYLE_PROPERTIES round = D2D1::StrokeStyleProperties(
          D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
      factory->CreateStrokeStyle(round, nullptr, 0, &resources.round);
      resources.mic = make_mic(factory.Get());
      resources.muted_mic = make_muted_mic(factory.Get());
    }
  }
  return resources.brush != nullptr;
}

bool lay_out(Text& line, const std::wstring& text, const wchar_t* family, float size,
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

// A soft glow round a rounded box: rings growing outwards, fading.
void draw_glow(ID2D1DeviceContext* canvas, const D2D1_ROUNDED_RECT& box, std::uint32_t rgb,
               float alpha, float blur) {
  ID2D1SolidColorBrush* brush = resources.brush.Get();
  constexpr int kRings = 5;
  for (int ring = 1; ring <= kRings; ++ring) {
    const float grow = blur * float(ring) / float(kRings);
    const float fade = 1.0f - float(ring - 1) / float(kRings);
    brush->SetColor(color_of(rgb, alpha * fade * 0.45f));
    const D2D1_ROUNDED_RECT wide =
        D2D1::RoundedRect(D2D1::RectF(box.rect.left - grow, box.rect.top - grow,
                                      box.rect.right + grow, box.rect.bottom + grow),
                          box.radiusX + grow, box.radiusY + grow);
    canvas->DrawRoundedRectangle(wide, brush, blur / float(kRings));
  }
}

// A text with the design's shadow (0 1px 2px, black at 0.9) under it.
void draw_text(ID2D1DeviceContext* canvas, const Text& line, D2D1_POINT_2F at, D2D1_COLOR_F color,
               float scale) {
  ID2D1SolidColorBrush* brush = resources.brush.Get();
  brush->SetColor(D2D1::ColorF(0, 0, 0, 0.9f * color.a));
  canvas->DrawTextLayout(D2D1::Point2F(at.x, at.y + scale), line.layout.Get(), brush);
  brush->SetColor(color);
  canvas->DrawTextLayout(at, line.layout.Get(), brush);
}

// A glyph of the 24-unit box drawn `side` pixels wide, centred at (x, y).
void draw_glyph(ID2D1DeviceContext* canvas, ID2D1PathGeometry* glyph, float x, float y, float side,
                D2D1_COLOR_F color) {
  if (!glyph) {
    return;
  }
  D2D1_MATRIX_3X2_F previous;
  canvas->GetTransform(&previous);
  const float unit = side / 24.0f;
  canvas->SetTransform(D2D1::Matrix3x2F::Scale(unit, unit) *
                       D2D1::Matrix3x2F::Translation(x - side / 2.0f, y - side / 2.0f) * previous);
  resources.brush->SetColor(color);
  canvas->DrawGeometry(glyph, resources.brush.Get(), 2.0f, resources.round.Get());
  canvas->SetTransform(previous);
}

float mix(float from, float to, float share) {
  return from + (to - from) * share;
}

}  // namespace

void set(const State& state) {
  std::lock_guard<std::mutex> lock(state_mutex);
  latest = state;
}

void paint(ID2D1DeviceContext* canvas, float width, float height) {
  State state;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    state = latest;
  }
  const float watermark_right = branding::watermark::right_edge();
  if (!state.shown || !canvas || width <= 0.0f || height <= 0.0f || watermark_right <= 0.0f) {
    return;
  }
  if (!prepare(canvas)) {
    return;
  }
  const double now = now_seconds();
  const int tier = std::clamp(state.tier, 0, 2);
  if (tier != shown_tier) {
    // Not on the first frame: only a change flashes.
    tier_changed_at = shown_tier < 0 ? -100.0 : now;
    shown_tier = tier;
  }
  const bool motion = animations_on();
  const float scale = height / kReferenceHeight;
  const auto px = [scale](float design) { return std::round(design * scale); };
  const bool talking = state.talking && !state.muted;
  const Look& look = state.muted ? kMuted : talking ? (tier == 2 ? kShout : kTalk) : kIdle;
  const std::wstring label = state.muted ? kMutedLabel : kTierLabels[tier];
  if (!lay_out(label_text, label, hud::text::kCinzel, std::max(8.0f, px(kLabelSize)),
               DWRITE_FONT_WEIGHT_SEMI_BOLD, hud::text::cinzel_fallback(), kLabelSpacing * scale)) {
    return;
  }
  const bool show_key = state.push_to_talk && !state.muted && !state.key.empty();
  const bool show_pips = !state.muted && !show_key;
  if (show_key) {
    lay_out(key_text, state.key, hud::text::kGaramond, std::max(8.0f, px(kKeySize)),
            DWRITE_FONT_WEIGHT_BOLD, hud::text::garamond_fallback(), 0.0f);
  }
  // The parts from left to right and the pill's width.
  const float bar = std::max(2.0f, px(kBarWidth));
  const float bar_gap = std::max(1.0f, px(kBarGap));
  const float gap = px(kPartGap);
  const float level_width = kLevelBars * bar + (kLevelBars - 1) * bar_gap;
  const float pips_width = 3 * bar + 2 * bar_gap;
  const float key_pad = px(5.0f);
  const float key_width =
      show_key && key_text.layout ? key_text.metrics.width + 2.0f * key_pad + px(2.0f) : 0.0f;
  float pill_width =
      px(kPadLeft) + px(kDisc) + gap + label_text.metrics.widthIncludingTrailingWhitespace;
  if (talking) {
    pill_width += gap + level_width;
  }
  if (show_pips) {
    pill_width += gap + pips_width;
  }
  if (show_key && key_text.layout) {
    pill_width += gap + key_width;
  }
  pill_width += px(kPadRight);
  const float pill_height = px(kPillHeight);
  const float middle = std::round(branding::watermark::middle());
  // The rule between the watermark and the pill.
  const float rule_x = std::round(watermark_right + px(kRuleGap));
  ID2D1SolidColorBrush* brush = resources.brush.Get();
  brush->SetColor(color_of(kRule, kRuleAlpha));
  canvas->FillRectangle(
      D2D1::RectF(rule_x, middle - px(kRuleHeight) / 2.0f, rule_x + std::max(1.0f, scale),
                  middle + px(kRuleHeight) / 2.0f),
      brush);
  const float left = rule_x + std::max(1.0f, scale) + px(kRuleGap);
  const float top = middle - pill_height / 2.0f;
  const D2D1_ROUNDED_RECT pill =
      D2D1::RoundedRect(D2D1::RectF(left, top, left + pill_width, top + pill_height),
                        pill_height / 2.0f, pill_height / 2.0f);
  if (look.glow != 0) {
    draw_glow(canvas, pill, look.glow, look.glow_alpha, px(look.glow_blur));
  }
  // The fill: its colour on the left fading to the dark ground on the right.
  ComPtr<ID2D1GradientStopCollection> stops;
  const D2D1_GRADIENT_STOP fill_stops[] = {{0.0f, color_of(look.fill_from, look.fill_from_alpha)},
                                           {1.0f, color_of(0x0C0906, look.fill_to_alpha)}};
  canvas->CreateGradientStopCollection(fill_stops, 2, &stops);
  ComPtr<ID2D1LinearGradientBrush> fill;
  if (stops) {
    canvas->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(left, top),
                                            D2D1::Point2F(left + pill_width, top)),
        stops.Get(), &fill);
  }
  if (fill) {
    canvas->FillRoundedRectangle(pill, fill.Get());
  }
  brush->SetColor(color_of(look.border, look.border_alpha));
  canvas->DrawRoundedRectangle(pill, brush, std::max(1.0f, scale));
  // The microphone's disc, with its ring pulsing out while the player is heard.
  const float disc = px(kDisc);
  const float disc_x = left + px(kPadLeft) + disc / 2.0f;
  if (look.ring != 0 && motion) {
    const float phase = float(std::fmod(now, double(kRingPeriod))) / kRingPeriod;
    // ease-out: fast at first.
    const float eased = 1.0f - (1.0f - phase) * (1.0f - phase);
    const float spread = px(look.ring_spread) * eased;
    brush->SetColor(color_of(look.ring, look.ring_alpha * (1.0f - eased)));
    canvas->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(disc_x, middle), disc / 2.0f + spread, disc / 2.0f + spread),
        brush);
  }
  const D2D1_ELLIPSE disc_shape =
      D2D1::Ellipse(D2D1::Point2F(disc_x, middle), disc / 2.0f, disc / 2.0f);
  brush->SetColor(color_of(look.disc_fill, look.disc_fill_alpha));
  canvas->FillEllipse(disc_shape, brush);
  brush->SetColor(color_of(look.disc_border, look.disc_border_alpha));
  canvas->DrawEllipse(disc_shape, brush, std::max(1.0f, scale));
  draw_glyph(canvas, state.muted ? resources.muted_mic.Get() : resources.mic.Get(), disc_x, middle,
             px(kGlyph), color_of(look.glyph, 1.0f));
  // The label, flashing once to the lightest gold when the range changed.
  float x = left + px(kPadLeft) + disc + gap;
  const float flash_age = float(now - tier_changed_at);
  float flash = 0.0f;
  if (flash_age >= 0.0f && flash_age < kFlashSeconds) {
    flash = std::sin(flash_age / kFlashSeconds * kPi);
  }
  D2D1_COLOR_F text = color_of(look.text, 1.0f);
  const D2D1_COLOR_F bright = color_of(0xF6EDD8, 1.0f);
  text.r = mix(text.r, bright.r, flash);
  text.g = mix(text.g, bright.g, flash);
  text.b = mix(text.b, bright.b, flash);
  const float label_top = std::round(middle - label_text.metrics.height / 2.0f);
  draw_text(canvas, label_text, D2D1::Point2F(x, label_top), text, scale);
  x += label_text.metrics.widthIncludingTrailingWhitespace;
  // The level bars, following the microphone.
  if (talking) {
    x += gap;
    // The design's motion at its quietest, taller with a louder voice.
    const float strength = 0.4f + 0.6f * std::clamp(state.level, 0.0f, 1.0f);
    for (int index = 0; index < kLevelBars; ++index) {
      float share = 0.55f;
      if (motion) {
        const float phase =
            float(std::fmod(now + kLevelDelays[index], double(kLevelPeriod))) / kLevelPeriod;
        share = 0.5f - 0.5f * std::cos(phase * 2.0f * kPi);
      }
      const float high = px(kLevelLow) + (px(kLevelHigh) - px(kLevelLow)) * share * strength;
      brush->SetColor(color_of(look.level, 1.0f));
      canvas->FillRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(x, middle - high / 2.0f, x + bar, middle + high / 2.0f),
                            bar / 2.0f, bar / 2.0f),
          brush);
      x += bar + bar_gap;
    }
    x -= bar_gap;
  }
  // The range's pips, bottom aligned: 1 to 3 lit.
  if (show_pips) {
    x += gap;
    const float bottom = middle + px(kPipHeights[2]) / 2.0f;
    for (int index = 0; index < 3; ++index) {
      const float high = px(kPipHeights[index]);
      const bool lit = index <= tier;
      brush->SetColor(lit ? color_of(look.pip_on, 1.0f) : color_of(kPipOff, kPipOffAlpha));
      canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, bottom - high, x + bar, bottom),
                                                     std::max(1.0f, scale), std::max(1.0f, scale)),
                                   brush);
      x += bar + bar_gap;
    }
  }
  // The push-to-talk key's cap in place of the pips.
  if (show_key && key_text.layout) {
    x += gap + px(2.0f);
    const float cap_high = key_text.metrics.height + 2.0f * px(1.0f) + px(1.0f);
    const D2D1_ROUNDED_RECT cap =
        D2D1::RoundedRect(D2D1::RectF(x, middle - cap_high / 2.0f, x + key_width - px(2.0f),
                                      middle + cap_high / 2.0f),
                          px(3.0f), px(3.0f));
    brush->SetColor(D2D1::ColorF(0, 0, 0, 0.35f));
    canvas->FillRoundedRectangle(cap, brush);
    brush->SetColor(color_of(kKeyBorder, kKeyBorderAlpha));
    canvas->DrawRoundedRectangle(cap, brush, std::max(1.0f, scale));
    // The cap's thicker bottom edge.
    canvas->FillRectangle(
        D2D1::RectF(cap.rect.left + px(3.0f), cap.rect.bottom - std::max(1.0f, scale) * 1.5f,
                    cap.rect.right - px(3.0f), cap.rect.bottom + std::max(1.0f, scale) * 0.5f),
        brush);
    draw_text(canvas, key_text,
              D2D1::Point2F(x + key_pad, std::round(middle - key_text.metrics.height / 2.0f)),
              color_of(kKeyText, 1.0f), scale);
  }
}

}  // namespace kcds::runtime::voice::indicator
