#include "render_view.hpp"

// std::min and std::max, not the Windows macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::render {

namespace {

namespace integration = kcd2s::integration;
namespace sdk = kcd2s::sdk;
using Microsoft::WRL::ComPtr;

constexpr char kAnchorName[] = "kcds_render_view";
// Frames presented after the camera is made before the pixels are taken: the engine renders a
// frame or two behind its update, so the frame then shows the camera for sure.
constexpr int kFilmPresents = 5;
// Frames presented after the camera ended that still show the kept frame.
constexpr int kRestorePresents = 3;
// The longest a phase may wait for the render thread (a minimised window presents nothing).
constexpr double kPhaseTimeout = 5.0;
// The base64 text of a PNG in each renderViewChunk (a runtime message holds 64 KiB at most).
constexpr std::size_t kChunkBytes = 48 * 1024;

enum class Phase {
  Idle,
  Saving,     // the render thread keeps the next frame
  Saved,      // the engine thread makes the camera
  Filming,    // frames go by; the kept one is shown
  Filmed,     // the pixels are taken; the engine thread ends the camera
  Restoring,  // the game's view comes back behind the kept frame
};

struct Request {
  double id = 0;
  camera::Pose pose;
  double fov = 0;  // degrees, 0 the game's own
  int width = 0;   // 0 the frame's own
};

struct Picture {
  double id = 0;
  int width = 0;
  int height = 0;
  int scaled_width = 0;
  std::vector<std::uint8_t> bgra;  // width * height * 4, top row first
  std::string failure;
};

camera::Natives natives;
Logger log_line = nullptr;

std::mutex mutex;
std::deque<Request> queue;
std::deque<Picture> finished;
std::atomic<Phase> phase{Phase::Idle};
// The render in progress (engine thread), and when its phase began.
std::optional<Request> current;
double phase_started = 0;
double current_fov = 60;
camera::AnchorView view(kAnchorName);
// The render being filmed, for the render thread (set before the phase moves to Filming).
std::atomic<double> filming_id{0};
std::atomic<int> filming_width{0};

// Render thread only.
ComPtr<ID3D11Texture2D> kept;
int presents = 0;
Picture taken;

double number(const sdk::Value& message, const char* name, double fallback) {
  const sdk::Value* value = integration::script_field(message, name);
  if (!value) {
    return fallback;
  }
  if (value->tag() == sdk::Tag::Double) {
    return value->as_double();
  }
  if (value->tag() == sdk::Tag::Int) {
    return double(value->as_int());
  }
  return fallback;
}

bool point(const sdk::Value& message, const char* name, double out[3]) {
  const sdk::Value* value = integration::script_field(message, name);
  if (!value || value->tag() != sdk::Tag::Map) {
    return false;
  }
  out[0] = number(*value, "x", 0);
  out[1] = number(*value, "y", 0);
  out[2] = number(*value, "z", 0);
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

void fail(double id, const char* reason) {
  Picture picture;
  picture.id = id;
  picture.failure = reason;
  std::lock_guard<std::mutex> lock(mutex);
  finished.push_back(std::move(picture));
}

// One frame's pixels as BGRA, 8 bits each; false for a format this does not read.
bool to_bgra(DXGI_FORMAT format, const std::uint8_t* row, int width, std::uint8_t* out) {
  for (int x = 0; x < width; ++x) {
    const std::uint8_t* pixel = row + x * 4;
    std::uint8_t* target = out + x * 4;
    switch (format) {
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8X8_UNORM:
        target[0] = pixel[0];
        target[1] = pixel[1];
        target[2] = pixel[2];
        break;
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        target[0] = pixel[2];
        target[1] = pixel[1];
        target[2] = pixel[0];
        break;
      case DXGI_FORMAT_R10G10B10A2_UNORM: {
        std::uint32_t packed = 0;
        std::memcpy(&packed, pixel, sizeof(packed));
        target[2] = std::uint8_t(((packed >> 0) & 0x3FF) >> 2);
        target[1] = std::uint8_t(((packed >> 10) & 0x3FF) >> 2);
        target[0] = std::uint8_t(((packed >> 20) & 0x3FF) >> 2);
        break;
      }
      default:
        return false;
    }
    target[3] = 255;
  }
  return true;
}

// The back buffer's pixels into `taken` (a staging copy, read at once: one wait for the GPU).
void take_pixels(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* back) {
  ComPtr<ID3D11Texture2D> texture;
  back->QueryInterface(IID_PPV_ARGS(&texture));
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
    taken.failure = "the frame could not be copied";
    return;
  }
  context->CopyResource(staging.Get(), back);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    taken.failure = "the frame could not be read";
    return;
  }
  taken.width = int(desc.Width);
  taken.height = int(desc.Height);
  taken.bgra.resize(std::size_t(desc.Width) * desc.Height * 4);
  bool readable = true;
  for (UINT y = 0; y < desc.Height && readable; ++y) {
    const auto* row =
        static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch;
    readable = to_bgra(desc.Format, row, int(desc.Width),
                       taken.bgra.data() + std::size_t(y) * desc.Width * 4);
  }
  context->Unmap(staging.Get(), 0);
  if (!readable) {
    taken.bgra.clear();
    taken.failure = "the frame's format is not one it reads";
    if (log_line) {
      log_line("RENDER frame format %u not read", unsigned(desc.Format));
    }
  }
}

// The PNG of a picture, scaled to its width (keeping the aspect), as base64; empty on failure.
std::string encode(const Picture& picture, int& out_width, int& out_height) {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::string text;
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    const UINT stride = UINT(picture.width) * 4;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateBitmapFromMemory(
                  UINT(picture.width), UINT(picture.height), GUID_WICPixelFormat32bppBGRA, stride,
                  UINT(picture.bgra.size()), const_cast<BYTE*>(picture.bgra.data()), &bitmap));
    ComPtr<IWICBitmapSource> source = bitmap;
    out_width = picture.width;
    out_height = picture.height;
    if (ok && picture.scaled_width > 0 && picture.scaled_width != picture.width) {
      out_width = picture.scaled_width;
      out_height =
          std::max(1, int(std::lround(double(picture.height) * out_width / picture.width)));
      ComPtr<IWICBitmapScaler> scaler;
      ok = SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
           SUCCEEDED(scaler->Initialize(bitmap.Get(), UINT(out_width), UINT(out_height),
                                        WICBitmapInterpolationModeFant));
      source = scaler;
    }
    ok = ok && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
         SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
         SUCCEEDED(frame->Initialize(nullptr)) &&
         SUCCEEDED(frame->SetSize(UINT(out_width), UINT(out_height)));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&format)) &&
         SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
    HGLOBAL memory = nullptr;
    if (ok && SUCCEEDED(GetHGlobalFromStream(stream.Get(), &memory))) {
      const auto* bytes = static_cast<const std::uint8_t*>(GlobalLock(memory));
      const std::size_t size = GlobalSize(memory);
      // Test kits only (KCDS_TEST_HOOKS): the PNG also goes to %TEMP%\kcds-render-<request>.png.
      wchar_t temp[MAX_PATH];
      if (GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) != 0 &&
          GetTempPathW(MAX_PATH, temp) != 0) {
        const std::wstring file = std::wstring(temp) + L"kcds-render-" +
                                  std::to_wstring(static_cast<long long>(picture.id)) + L".png";
        HANDLE handle =
            CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
          DWORD written = 0;
          WriteFile(handle, bytes, DWORD(size), &written, nullptr);
          CloseHandle(handle);
        }
      }
      static const char kDigits[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      text.reserve((size + 2) / 3 * 4);
      for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t chunk = (std::uint32_t(bytes[i]) << 16) |
                                    (i + 1 < size ? std::uint32_t(bytes[i + 1]) << 8 : 0) |
                                    (i + 2 < size ? std::uint32_t(bytes[i + 2]) : 0);
        text.push_back(kDigits[(chunk >> 18) & 63]);
        text.push_back(kDigits[(chunk >> 12) & 63]);
        text.push_back(i + 1 < size ? kDigits[(chunk >> 6) & 63] : '=');
        text.push_back(i + 2 < size ? kDigits[chunk & 63] : '=');
      }
      GlobalUnlock(memory);
    }
  }
  if (SUCCEEDED(initialized)) {
    CoUninitialize();
  }
  return text;
}

void begin_phase(Phase next, double seconds) {
  phase = next;
  phase_started = seconds;
}

void end_camera() {
  if (view.active()) {
    view.end();
  }
  view = camera::AnchorView(kAnchorName);
}

}  // namespace

void install(const camera::Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "renderView") {
    return false;
  }
  // The script's checks ran in Client/resources/render.cjs; a malformed message fails.
  Request request;
  request.id = number(message, "request", 0);
  double position[3];
  if (!point(message, "position", position)) {
    fail(request.id, "invalid pose");
    return true;
  }
  double target[3];
  if (point(message, "lookAt", target)) {
    const double fx = target[0] - position[0];
    const double fy = target[1] - position[1];
    const double fz = target[2] - position[2];
    request.pose.yaw = std::atan2(-fx, fy) * 180 / 3.14159265358979323846;
    request.pose.pitch =
        std::clamp(std::atan2(fz, std::hypot(fx, fy)) * 180 / 3.14159265358979323846, -89.0, 89.0);
    request.pose.roll = number(message, "roll", 0);
  } else {
    const sdk::Value* rotation = integration::script_field(message, "rotation");
    if (!rotation || rotation->tag() != sdk::Tag::Map) {
      fail(request.id, "invalid pose");
      return true;
    }
    request.pose = camera::pose_of_rotation(number(*rotation, "x", 0), number(*rotation, "y", 0),
                                            number(*rotation, "z", 0), number(*rotation, "w", 1));
  }
  request.pose.x = position[0];
  request.pose.y = position[1];
  request.pose.z = position[2];
  request.fov = std::clamp(number(message, "fov", 0), 0.0, 150.0);
  request.width = int(std::clamp(number(message, "width", 0), 0.0, 7680.0));
  std::lock_guard<std::mutex> lock(mutex);
  queue.push_back(request);
  return true;
}

void tick(bool in_world, double seconds) {
  const Phase now = phase.load();
  if (now == Phase::Idle) {
    current.reset();
    std::optional<Request> next;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!queue.empty()) {
        next = queue.front();
        queue.pop_front();
      }
    }
    if (!next) {
      return;
    }
    if (!in_world) {
      fail(next->id, "not in a world");
      return;
    }
    current = next;
    begin_phase(Phase::Saving, seconds);
    return;
  }
  const bool late = seconds - phase_started > kPhaseTimeout;
  if (!in_world || late) {
    // The render thread stops showing the kept frame once it sees Idle.
    end_camera();
    if (current) {
      fail(current->id, in_world ? "timed out" : "the world changed");
    }
    current.reset();
    phase = Phase::Idle;
    return;
  }
  if (now == Phase::Saved) {
    camera::Pose previous;
    double previous_fov = 60;
    if (!view.begin(natives, &previous, &previous_fov)) {
      end_camera();
      fail(current->id, "the view could not be made");
      current.reset();
      begin_phase(Phase::Restoring, seconds);
      return;
    }
    current_fov = current->fov > 0 ? current->fov : previous_fov;
    view.place(current->pose, current_fov);
    filming_id = current->id;
    filming_width = current->width;
    begin_phase(Phase::Filming, seconds);
  } else if (now == Phase::Filming) {
    // Kept at its pose every frame, as a shot is.
    view.place(current->pose, current_fov);
  } else if (now == Phase::Filmed) {
    end_camera();
    begin_phase(Phase::Restoring, seconds);
  }
}

void on_present(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* back) {
  const Phase now = phase.load();
  if (now == Phase::Idle || !device || !context || !back) {
    if (now == Phase::Idle) {
      kept.Reset();
    }
    return;
  }
  if (now == Phase::Saving) {
    ComPtr<ID3D11Texture2D> texture;
    back->QueryInterface(IID_PPV_ARGS(&texture));
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    kept.Reset();
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &kept))) {
      context->CopyResource(kept.Get(), back);
    }
    presents = 0;
    taken = Picture();
    phase = Phase::Saved;
    return;
  }
  if (now == Phase::Filming) {
    presents += 1;
    if (presents >= kFilmPresents) {
      take_pixels(device, context, back);
      taken.id = filming_id.load();
      taken.scaled_width = filming_width.load();
      phase = Phase::Filmed;
    }
  }
  if (now == Phase::Restoring) {
    presents += 1;
    if (presents >= kFilmPresents + kRestorePresents) {
      Picture picture = std::move(taken);
      taken = Picture();
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (picture.id != 0) {
          finished.push_back(std::move(picture));
        }
      }
      phase = Phase::Idle;
    }
  }
  // The player sees the kept frame while the camera is elsewhere.
  if (kept) {
    context->CopyResource(back, kept.Get());
  }
}

void pump(const RuntimeSender& send) {
  std::deque<Picture> ready;
  {
    std::lock_guard<std::mutex> lock(mutex);
    ready.swap(finished);
  }
  for (Picture& picture : ready) {
    if (picture.failure.empty() && picture.bgra.empty()) {
      picture.failure = "no frame was taken";
    }
    if (!picture.failure.empty()) {
      send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("renderViewFailed"))},
                                      {"request", sdk::Value(picture.id)},
                                      {"reason", sdk::Value(picture.failure)}}));
      continue;
    }
    int width = 0;
    int height = 0;
    std::string png = encode(picture, width, height);
    if (png.empty()) {
      send(sdk::Value(
          sdk::Value::Map{{"op", sdk::Value(std::string("renderViewFailed"))},
                          {"request", sdk::Value(picture.id)},
                          {"reason", sdk::Value(std::string("the PNG could not be made"))}}));
      continue;
    }
    if (log_line) {
      log_line("RENDER view %.0f: %dx%d PNG, %zu base64 bytes", picture.id, width, height,
               png.size());
    }
    // A runtime message holds at most 64 KiB: the PNG goes in pieces, then the answer.
    double pieces = 0;
    for (std::size_t at = 0; at < png.size(); at += kChunkBytes) {
      send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("renderViewChunk"))},
                                      {"request", sdk::Value(picture.id)},
                                      {"data", sdk::Value(png.substr(at, kChunkBytes))}}));
      pieces += 1;
    }
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("renderViewDone"))},
                                    {"request", sdk::Value(picture.id)},
                                    {"chunks", sdk::Value(pieces)},
                                    {"width", sdk::Value(double(width))},
                                    {"height", sdk::Value(double(height))}}));
  }
}

}  // namespace kcds::runtime::render
