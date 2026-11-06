// The HUD's own text (the nameplates, the watermark, the voice indicator): the design's fonts,
// Cinzel and EB Garamond, from the menu's font files (ui/menu/fonts next to the client DLL), loaded
// once into DirectWrite; the system's serif when they cannot be. Render thread.
#pragma once

#include <dwrite_3.h>
#include <wrl/client.h>

#include <string>

namespace kcds::runtime::hud::text {

inline constexpr wchar_t kCinzel[] = L"Cinzel";
inline constexpr wchar_t kGaramond[] = L"EB Garamond";

// Loads the fonts once; false without DirectWrite (nothing can be laid out then).
bool load();

// A family's fallback: its latin-ext letters from the menu's ext files, the rest the system's.
// Null before load() or when it could not be made.
IDWriteFontFallback* cinzel_fallback();
IDWriteFontFallback* garamond_fallback();

// A one-line text laid out at a size (pixels), its letters `spacing` pixels apart; null when it
// cannot be. The family is one of the above, or the system's serif when the menu's fonts are
// missing.
Microsoft::WRL::ComPtr<IDWriteTextLayout> layout(const std::wstring& text, const wchar_t* family,
                                                 float size, DWRITE_FONT_WEIGHT weight,
                                                 DWRITE_FONT_STYLE style,
                                                 IDWriteFontFallback* fallback, float spacing);

}  // namespace kcds::runtime::hud::text
