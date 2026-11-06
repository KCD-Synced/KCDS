#include "hud_text.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <vector>

namespace kcds::runtime::hud::text {
namespace {

using Microsoft::WRL::ComPtr;

// The latin-ext subsets' letters (ui/menu/fonts/fonts.css), which the latin subsets lack.
constexpr DWRITE_UNICODE_RANGE kExtRanges[] = {
    {0x0100, 0x02BA}, {0x02BD, 0x02C5}, {0x02C7, 0x02CC}, {0x02CE, 0x02D7},
    {0x02DD, 0x02FF}, {0x1D00, 0x1DBF}, {0x1E00, 0x1E9F}, {0x1EF2, 0x1EFF},
    {0x20A0, 0x20AB}, {0x20AD, 0x20C4}, {0x2C60, 0x2C7F}, {0xA720, 0xA7FF},
};
constexpr wchar_t kSystemSerif[] = L"Georgia";

// What the HUD's text draws with, loaded once.
struct Fonts {
  bool tried = false;
  ComPtr<IDWriteFactory5> factory;
  ComPtr<IDWriteInMemoryFontFileLoader> loader;
  ComPtr<IDWriteFontCollection1> base;
  ComPtr<IDWriteFontFallback> cinzel_fallback;
  ComPtr<IDWriteFontFallback> garamond_fallback;
};
Fonts fonts;

// The folder of the client DLL, where ui/menu/fonts is.
std::wstring module_folder() {
  HMODULE module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&module_folder), &module);
  wchar_t path[MAX_PATH] = {};
  const DWORD length = module ? GetModuleFileNameW(module, path, MAX_PATH) : 0;
  std::wstring folder(path, length);
  const std::size_t slash = folder.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : folder.substr(0, slash + 1);
}

// A WOFF2 file of ui/menu/fonts unpacked into a font file of the in-memory loader; null when it
// cannot be.
ComPtr<IDWriteFontFile> font_file(const std::wstring& folder, const wchar_t* name) {
  std::ifstream stream(folder + L"ui\\menu\\fonts\\" + name, std::ios::binary);
  const std::vector<char> packed((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());
  ComPtr<IDWriteFontFileStream> unpacked;
  if (packed.empty() ||
      FAILED(fonts.factory->UnpackFontFile(DWRITE_CONTAINER_TYPE_WOFF2, packed.data(),
                                           UINT32(packed.size()), &unpacked))) {
    return nullptr;
  }
  UINT64 size = 0;
  const void* start = nullptr;
  void* context = nullptr;
  if (FAILED(unpacked->GetFileSize(&size)) ||
      FAILED(unpacked->ReadFileFragment(&start, 0, size, &context))) {
    return nullptr;
  }
  ComPtr<IDWriteFontFile> file;
  const HRESULT made = fonts.loader->CreateInMemoryFontFileReference(fonts.factory.Get(), start,
                                                                     UINT32(size), nullptr, &file);
  unpacked->ReleaseFileFragment(context);
  return SUCCEEDED(made) ? file : nullptr;
}

// A collection of the menu's font files; null when none loads.
ComPtr<IDWriteFontCollection1> collection_of(const std::wstring& folder,
                                             std::initializer_list<const wchar_t*> names) {
  ComPtr<IDWriteFontSetBuilder1> builder;
  if (FAILED(fonts.factory->CreateFontSetBuilder(&builder))) {
    return nullptr;
  }
  bool added = false;
  for (const wchar_t* name : names) {
    const ComPtr<IDWriteFontFile> file = font_file(folder, name);
    if (file && SUCCEEDED(builder->AddFontFile(file.Get()))) {
      added = true;
    }
  }
  ComPtr<IDWriteFontSet> set;
  ComPtr<IDWriteFontCollection1> collection;
  if (!added || FAILED(builder->CreateFontSet(&set)) ||
      FAILED(fonts.factory->CreateFontCollectionFromFontSet(set.Get(), &collection))) {
    return nullptr;
  }
  return collection;
}

// The fallback of a family: its latin-ext letters from the ext subsets, the rest the system's.
ComPtr<IDWriteFontFallback> fallback_of(const wchar_t* family, IDWriteFontCollection1* ext) {
  ComPtr<IDWriteFontFallbackBuilder> builder;
  ComPtr<IDWriteFontFallback> system;
  if (FAILED(fonts.factory->CreateFontFallbackBuilder(&builder)) ||
      FAILED(fonts.factory->GetSystemFontFallback(&system))) {
    return nullptr;
  }
  if (ext) {
    const wchar_t* families[] = {family};
    builder->AddMapping(kExtRanges, UINT32(std::size(kExtRanges)), families, 1, ext, nullptr,
                        nullptr, 1.0f);
  }
  builder->AddMappings(system.Get());
  ComPtr<IDWriteFontFallback> fallback;
  return SUCCEEDED(builder->CreateFontFallback(&fallback)) ? fallback : nullptr;
}

}  // namespace

bool load() {
  if (fonts.tried) {
    return fonts.factory != nullptr;
  }
  fonts.tried = true;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                 reinterpret_cast<IUnknown**>(fonts.factory.GetAddressOf())))) {
    return false;
  }
  if (FAILED(fonts.factory->CreateInMemoryFontFileLoader(&fonts.loader)) ||
      FAILED(fonts.factory->RegisterFontFileLoader(fonts.loader.Get()))) {
    // The system's serif draws then.
    fonts.loader.Reset();
    return true;
  }
  const std::wstring folder = module_folder();
  fonts.base =
      collection_of(folder, {L"Cinzel-normal-latin.woff2", L"EBGaramond-normal-latin.woff2",
                             L"EBGaramond-italic-latin.woff2"});
  const ComPtr<IDWriteFontCollection1> cinzel_ext =
      collection_of(folder, {L"Cinzel-normal-latin-ext.woff2"});
  const ComPtr<IDWriteFontCollection1> garamond_ext = collection_of(
      folder, {L"EBGaramond-normal-latin-ext.woff2", L"EBGaramond-italic-latin-ext.woff2"});
  fonts.cinzel_fallback = fallback_of(kCinzel, cinzel_ext.Get());
  fonts.garamond_fallback = fallback_of(kGaramond, garamond_ext.Get());
  return true;
}

IDWriteFontFallback* cinzel_fallback() {
  return fonts.cinzel_fallback.Get();
}

IDWriteFontFallback* garamond_fallback() {
  return fonts.garamond_fallback.Get();
}

ComPtr<IDWriteTextLayout> layout(const std::wstring& text, const wchar_t* family, float size,
                                 DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
                                 IDWriteFontFallback* fallback, float spacing) {
  if (!fonts.factory) {
    return nullptr;
  }
  const bool menu_font = fonts.base != nullptr;
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(fonts.factory->CreateTextFormat(
          menu_font ? family : kSystemSerif, menu_font ? fonts.base.Get() : nullptr, weight, style,
          DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &format))) {
    return nullptr;
  }
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  ComPtr<IDWriteTextFormat1> format1;
  if (menu_font && fallback && SUCCEEDED(format.As(&format1))) {
    format1->SetFontFallback(fallback);
  }
  ComPtr<IDWriteTextLayout> made;
  if (FAILED(fonts.factory->CreateTextLayout(text.data(), UINT32(text.size()), format.Get(),
                                             100000.0f, 1000.0f, &made))) {
    return nullptr;
  }
  ComPtr<IDWriteTextLayout1> layout1;
  if (spacing > 0.0f && SUCCEEDED(made.As(&layout1))) {
    layout1->SetCharacterSpacing(0.0f, spacing, 0.0f, {0, UINT32(text.size())});
  }
  return made;
}

}  // namespace kcds::runtime::hud::text
