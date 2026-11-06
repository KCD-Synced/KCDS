#include "editor_overlay.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/integration/script_channel.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace kcds::runtime::architect::overlay {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
using Microsoft::WRL::ComPtr;

constexpr float kStageWidth = 1920.0f;
constexpr float kStageHeight = 1080.0f;
// What the editor may send: lines, pictures kept and one picture's size.
constexpr std::size_t kMaxRuns = 600;
constexpr std::size_t kMaxPictures = 64;
constexpr std::size_t kMaxKept = 400;
constexpr std::size_t kMaxPictureBytes = 2 * 1024 * 1024;

struct Run {
  float x = 0, y = 0, w = 0, h = 0;
  std::wstring text;
  float size = 16;
  std::uint32_t color = 0xe4e6e9;
  char align = 'l';
  bool bold = false;
};

struct Placed {
  std::string key;
  float x = 0, y = 0, w = 0, h = 0;
};

// What the editor shows now (any thread, under the mutex).
std::mutex state_mutex;
std::vector<Run> runs;
std::vector<Placed> placed;
// Pictures by key: their PNG bytes while they come, whole ones waiting to be decoded.
std::map<std::string, std::vector<std::uint8_t>> arriving;
std::map<std::string, std::vector<std::uint8_t>> whole;
bool cleared = false;
// The editor's stage the text and pictures are placed on (theme.cjs stageFor: the window's shape).
float stage_width_now = kStageWidth;
float stage_height_now = kStageHeight;

// Render thread only: the canvas's bitmaps and the text formats by size and weight.
std::map<std::string, ComPtr<ID2D1Bitmap>> bitmaps;
ComPtr<IDWriteFactory> text_factory;
std::map<std::pair<int, bool>, ComPtr<IDWriteTextFormat>> formats;
ComPtr<ID2D1SolidColorBrush> brush;
ID2D1DeviceContext* brush_canvas = nullptr;

// ---- The pictures kept on disk ----

std::mutex disk_mutex;

std::filesystem::path picture_folder() {
  wchar_t local[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return std::filesystem::path(local) / L"KCDS" / L"architect" / L"pictures";
}

std::string hash_of(const std::string& key) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char c : key) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
  return text;
}

// The index's lines: hash -> key.
std::map<std::string, std::string> read_index(const std::filesystem::path& folder) {
  std::map<std::string, std::string> index;
  std::ifstream file(folder / L"index.txt");
  std::string line;
  while (std::getline(file, line)) {
    const std::size_t space = line.find(' ');
    if (space == 16 && line.size() > 17) {
      index[line.substr(0, 16)] = line.substr(17);
    }
  }
  return index;
}

void keep_on_disk(const std::string& key, const std::vector<std::uint8_t>& png) {
  const std::filesystem::path folder = picture_folder();
  if (folder.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(disk_mutex);
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  const std::string hash = hash_of(key);
  std::ofstream file(folder / (hash + ".png"), std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
  if (!file) {
    return;
  }
  if (read_index(folder).count(hash) == 0) {
    std::ofstream index(folder / L"index.txt", std::ios::app);
    index << hash << ' ' << key << '\n';
  }
}

std::vector<std::uint8_t> read_from_disk(const std::string& key) {
  const std::filesystem::path folder = picture_folder();
  if (folder.empty()) {
    return {};
  }
  std::lock_guard<std::mutex> lock(disk_mutex);
  std::ifstream file(folder / (hash_of(key) + ".png"), std::ios::binary);
  if (!file) {
    return {};
  }
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
}

double number(const sdk::Value& value, const char* name, double fallback) {
  const sdk::Value* field = integration::script_field(value, name);
  if (!field) {
    return fallback;
  }
  if (field->tag() == sdk::Tag::Double) {
    return field->as_double();
  }
  if (field->tag() == sdk::Tag::Int) {
    return double(field->as_int());
  }
  return fallback;
}

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

std::wstring wide(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring out(std::size_t(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), length);
  return out;
}

std::vector<std::uint8_t> from_base64(const std::string& data) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') {
      return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
      return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
      return c - '0' + 52;
    }
    if (c == '+') {
      return 62;
    }
    if (c == '/') {
      return 63;
    }
    return -1;
  };
  std::vector<std::uint8_t> out;
  out.reserve(data.size() * 3 / 4);
  int bits = 0;
  int count = 0;
  for (const char c : data) {
    const int v = value(c);
    if (v < 0) {
      continue;
    }
    bits = (bits << 6) | v;
    count += 6;
    if (count >= 8) {
      count -= 8;
      out.push_back(std::uint8_t((bits >> count) & 0xff));
    }
  }
  return out;
}

void read_runs(const sdk::Value& message) {
  const sdk::Value* list = integration::script_field(message, "runs");
  const sdk::Value* given = integration::script_field(message, "stage");
  float given_width = kStageWidth;
  float given_height = kStageHeight;
  if (given && given->tag() == sdk::Tag::Map) {
    given_width = float(number(*given, "width", kStageWidth));
    given_height = float(number(*given, "height", kStageHeight));
  }
  std::vector<Run> made;
  if (list && list->tag() == sdk::Tag::Array) {
    for (const sdk::Value& item : list->as_array()) {
      if (made.size() >= kMaxRuns || item.tag() != sdk::Tag::Map) {
        break;
      }
      Run run;
      run.x = float(number(item, "x", 0));
      run.y = float(number(item, "y", 0));
      run.w = float(number(item, "w", 100));
      run.h = float(number(item, "h", 24));
      run.text = wide(text(item, "t"));
      run.size = float(number(item, "s", 16));
      run.color = std::uint32_t(number(item, "c", 0xe4e6e9));
      const std::string align = text(item, "a");
      run.align = align.empty() ? 'l' : align[0];
      const sdk::Value* bold = integration::script_field(item, "b");
      run.bold = bold && bold->tag() == sdk::Tag::Bool && bold->as_bool();
      made.push_back(std::move(run));
    }
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  runs = std::move(made);
  if (given_width > 0 && given_height > 0) {
    stage_width_now = given_width;
    stage_height_now = given_height;
  }
  cleared = false;
}

void read_pictures(const sdk::Value& message) {
  const sdk::Value* list = integration::script_field(message, "items");
  std::vector<Placed> made;
  if (list && list->tag() == sdk::Tag::Array) {
    for (const sdk::Value& item : list->as_array()) {
      if (made.size() >= kMaxPictures || item.tag() != sdk::Tag::Map) {
        break;
      }
      Placed picture;
      picture.key = text(item, "key");
      picture.x = float(number(item, "x", 0));
      picture.y = float(number(item, "y", 0));
      picture.w = float(number(item, "w", 0));
      picture.h = float(number(item, "h", 0));
      made.push_back(std::move(picture));
    }
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  placed = std::move(made);
}

void read_picture_data(const sdk::Value& message) {
  const std::string key = text(message, "key");
  const double offset = number(message, "offset", -1);
  const double total = number(message, "total", 0);
  if (key.empty() || offset < 0 || total <= 0 || total > double(kMaxPictureBytes)) {
    return;
  }
  const std::vector<std::uint8_t> piece = from_base64(text(message, "data"));
  std::lock_guard<std::mutex> lock(state_mutex);
  if (whole.size() + arriving.size() >= kMaxKept) {
    return;
  }
  std::vector<std::uint8_t>& bytes = arriving[key];
  if (offset == 0) {
    bytes.clear();
  }
  if (double(bytes.size()) != offset) {
    arriving.erase(key);
    return;
  }
  bytes.insert(bytes.end(), piece.begin(), piece.end());
  if (double(bytes.size()) >= total) {
    keep_on_disk(key, bytes);
    whole[key] = std::move(bytes);
    arriving.erase(key);
  }
}

// A PNG decoded into a bitmap of the canvas; null when it cannot be.
ComPtr<ID2D1Bitmap> decode(ID2D1DeviceContext* canvas, const std::vector<std::uint8_t>& png) {
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
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), DWORD(png.size()))) &&
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

IDWriteTextFormat* format_for(float pixels, bool bold) {
  if (!text_factory &&
      FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(text_factory.GetAddressOf())))) {
    return nullptr;
  }
  const int size = int(pixels + 0.5f) < 6 ? 6 : int(pixels + 0.5f);
  auto& format = formats[{size, bold}];
  if (!format) {
    const auto weight = bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    if (FAILED(text_factory->CreateTextFormat(L"Segoe UI", nullptr, weight,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              float(size), L"en-US", &format))) {
      return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    text_factory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
    format->SetTrimming(&trimming, ellipsis.Get());
  }
  return format.Get();
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "architectText") {
    read_runs(message);
  } else if (operation == "architectPictures") {
    read_pictures(message);
  } else if (operation == "architectPictureData") {
    read_picture_data(message);
  } else if (operation == "architectClear") {
    std::lock_guard<std::mutex> lock(state_mutex);
    runs.clear();
    placed.clear();
    cleared = true;
  } else {
    return false;
  }
  return true;
}

void paint(ID2D1DeviceContext* canvas, float width, float height) {
  std::vector<Run> lines;
  std::vector<Placed> pictures;
  std::map<std::string, std::vector<std::uint8_t>> fresh;
  float drawn_width = kStageWidth;
  float drawn_height = kStageHeight;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (cleared || (runs.empty() && placed.empty())) {
      return;
    }
    lines = runs;
    pictures = placed;
    drawn_width = stage_width_now;
    drawn_height = stage_height_now;
    fresh.swap(whole);
  }
  if (!canvas || height <= 0) {
    return;
  }
  for (auto& [key, png] : fresh) {
    bitmaps[key] = decode(canvas, png);
  }
  // A placed picture this session has not had: from the disk, once.
  for (const Placed& picture : pictures) {
    if (bitmaps.count(picture.key) == 0) {
      const std::vector<std::uint8_t> png = read_from_disk(picture.key);
      bitmaps[picture.key] = png.empty() ? nullptr : decode(canvas, png);
    }
  }
  if (!brush || brush_canvas != canvas) {
    brush.Reset();
    if (FAILED(canvas->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush))) {
      return;
    }
    brush_canvas = canvas;
  }
  // The editor's stage fills the window (its panels' clip makes it so, shell.cjs placePanels). It
  // has the window's shape, so both stretches are the same but for rounding, and while a resize is
  // on its way; the text keeps its shape at the smaller one, so it stays inside its box.
  const float scale_x = width / drawn_width;
  const float scale_y = height / drawn_height;
  const float scale = std::min(scale_x, scale_y);
  auto box = [&](float x, float y, float w, float h) {
    return D2D1::RectF(x * scale_x, y * scale_y, (x + w) * scale_x, (y + h) * scale_y);
  };
  for (const Placed& picture : pictures) {
    const auto found = bitmaps.find(picture.key);
    if (found == bitmaps.end() || !found->second) {
      continue;
    }
    // Fitted into its box, its shape kept, centred.
    const auto size = found->second->GetSize();
    if (size.width <= 0 || size.height <= 0) {
      continue;
    }
    // In window pixels, as the stage's box is stretched there.
    const auto room = box(picture.x, picture.y, picture.w, picture.h);
    const float room_w = room.right - room.left;
    const float room_h = room.bottom - room.top;
    const float fit = std::min(room_w / size.width, room_h / size.height);
    const float w = size.width * fit;
    const float h = size.height * fit;
    const float left = room.left + (room_w - w) / 2;
    const float top = room.top + (room_h - h) / 2;
    const auto place = D2D1::RectF(left, top, left + w, top + h);
    canvas->DrawBitmap(found->second.Get(), place, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  }
  for (const Run& line : lines) {
    IDWriteTextFormat* format = format_for(line.size * scale, line.bold);
    if (!format || line.text.empty()) {
      continue;
    }
    if (line.align == 'c') {
      format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (line.align == 'r') {
      format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else {
      format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    const float red = float((line.color >> 16) & 0xff) / 255.0f;
    const float green = float((line.color >> 8) & 0xff) / 255.0f;
    const float blue = float(line.color & 0xff) / 255.0f;
    brush->SetColor(D2D1::ColorF(red, green, blue, 1.0f));
    canvas->DrawText(line.text.data(), UINT32(line.text.size()), format,
                     box(line.x, line.y, line.w, line.h), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
}

std::vector<std::string> cached_keys() {
  std::vector<std::string> keys;
  const std::filesystem::path folder = picture_folder();
  if (folder.empty()) {
    return keys;
  }
  std::lock_guard<std::mutex> lock(disk_mutex);
  for (const auto& [hash, key] : read_index(folder)) {
    std::error_code error;
    if (std::filesystem::exists(folder / (hash + ".png"), error)) {
      keys.push_back(key);
    }
  }
  return keys;
}

float stage_height() {
  std::lock_guard<std::mutex> lock(state_mutex);
  return stage_height_now;
}

void reset() {
  std::lock_guard<std::mutex> lock(state_mutex);
  runs.clear();
  placed.clear();
  arriving.clear();
  whole.clear();
  cleared = true;
}

}  // namespace kcds::runtime::architect::overlay
