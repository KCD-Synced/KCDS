#include "render_view.hpp"

// std::min and std::max, not the Windows macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
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

#include "../native_ui/native_ui.hpp"

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
  bool ui = true;  // false: the resources' native screens left out
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
// The resources' native screens are hidden for the picture being filmed (engine thread).
bool screens_hidden = false;

// Render thread only. The copies are D3D12 command lists of KCDS's own on the game's queue,
// placed before the overlay's D3D11 work of the same present. The render thread never waits for
// the GPU: a list's allocator is used again, a picture's pixels read and an old texture let go
// only once KCDS's fence shows the GPU is past them. (Copies through D3D11 on 12 waited inside the
// present, and with the game's own frame settings that wait never ended: the game froze.)
constexpr int kListCount = 8;
struct List {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;
  UINT64 done_at = 0;  // the fence value its last submission reaches
};
struct Retired {
  UINT64 done_at = 0;
  ComPtr<ID3D12Resource> resource;
};
ID3D12Device* gpu_device = nullptr;
ComPtr<ID3D12Fence> fence;
UINT64 signalled = 0;
List lists[kListCount];
std::vector<Retired> retired;
// The kept frame (in the copy source state once filled) and the buffer a picture's pixels are
// read back through.
ComPtr<ID3D12Resource> kept;
bool kept_filled = false;
ComPtr<ID3D12Resource> readback;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
DXGI_FORMAT readback_format = DXGI_FORMAT_UNKNOWN;
// The fence value the picture's copy into `readback` reaches; 0 while none is asked.
UINT64 reading_at = 0;
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

// The picture's pixels from `readback` into `taken` (its copy is complete on the GPU).
void read_pixels() {
  const UINT width = UINT(footprint.Footprint.Width);
  const UINT height = footprint.Footprint.Height;
  void* data = nullptr;
  const D3D12_RANGE whole{0, SIZE_T(footprint.Footprint.RowPitch) * height};
  if (FAILED(readback->Map(0, &whole, &data))) {
    taken.failure = "the frame could not be read";
    return;
  }
  taken.width = int(width);
  taken.height = int(height);
  taken.bgra.resize(std::size_t(width) * height * 4);
  bool readable = true;
  for (UINT y = 0; y < height && readable; ++y) {
    const auto* row =
        static_cast<const std::uint8_t*>(data) + std::size_t(y) * footprint.Footprint.RowPitch;
    readable =
        to_bgra(readback_format, row, int(width), taken.bgra.data() + std::size_t(y) * width * 4);
  }
  const D3D12_RANGE nothing_written{0, 0};
  readback->Unmap(0, &nothing_written);
  if (!readable) {
    taken.bgra.clear();
    taken.failure = "the frame's format is not one it reads";
    if (log_line) {
      log_line("RENDER frame format %u not read", unsigned(readback_format));
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
  // The resources' screens a picture left out come back, still behind the kept frame
  // (kRestorePresents more presents show it).
  if (screens_hidden) {
    native_ui::show_screens_after_picture();
    screens_hidden = false;
  }
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
  const sdk::Value* ui = integration::script_field(message, "ui");
  request.ui = !(ui && ui->tag() == sdk::Tag::Bool && !ui->as_bool());
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
    // A picture without the resources' screens: hidden now, after the frame the player sees
    // meanwhile was kept, so the player never sees them go.
    if (!current->ui) {
      native_ui::hide_screens_for_picture();
      screens_hidden = true;
    }
    filming_id = current->id;
    filming_width = current->width;
    begin_phase(Phase::Filming, seconds);
  } else if (now == Phase::Filming) {
    // Kept at its pose every frame, as a shot is.
    view.place(current->pose, current_fov);
  } else if (now == Phase::Filmed) {
    // The picture went to the scripts already.
    end_camera();
    current.reset();
    begin_phase(Phase::Restoring, seconds);
  }
}

namespace {

// KCDS's fence and lists on this device (made again when the game's device changes).
bool ready_for(ID3D12Device* device) {
  if (device == gpu_device && fence) {
    return true;
  }
  fence.Reset();
  for (List& list : lists) {
    list = List();
  }
  retired.clear();
  kept.Reset();
  kept_filled = false;
  readback.Reset();
  reading_at = 0;
  signalled = 0;
  gpu_device = device;
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
    fence.Reset();
    return false;
  }
  return true;
}

// A list whose last submission the GPU finished, reset for recording; null when all are still in
// flight (the copy waits for a later present).
List* free_list(ID3D12Device* device, UINT64 completed) {
  for (List& list : lists) {
    if (list.done_at > completed) {
      continue;
    }
    if (!list.allocator) {
      if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&list.allocator))) ||
          FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, list.allocator.Get(),
                                           nullptr, IID_PPV_ARGS(&list.commands)))) {
        list = List();
        return nullptr;
      }
      return &list;
    }
    if (FAILED(list.allocator->Reset()) ||
        FAILED(list.commands->Reset(list.allocator.Get(), nullptr))) {
      return nullptr;
    }
    return &list;
  }
  return nullptr;
}

void submit(ID3D12CommandQueue* queue, List& list) {
  list.commands->Close();
  ID3D12CommandList* submitted[] = {list.commands.Get()};
  queue->ExecuteCommandLists(1, submitted);
  signalled += 1;
  queue->Signal(fence.Get(), signalled);
  list.done_at = signalled;
}

void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  commands->ResourceBarrier(1, &barrier);
}

// An old texture or buffer is let go once the GPU is past its last use.
void retire(ComPtr<ID3D12Resource>& resource) {
  if (resource) {
    retired.push_back({signalled, resource});
    resource.Reset();
  }
}

void release_retired(UINT64 completed) {
  std::erase_if(retired, [completed](const Retired& old) { return old.done_at <= completed; });
}

// The picture taken (or its failure) to the scripts.
void deliver_taken() {
  Picture picture = std::move(taken);
  taken = Picture();
  if (picture.id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex);
  finished.push_back(std::move(picture));
}

bool same_frame(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
  return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format &&
         a.SampleDesc.Count == b.SampleDesc.Count;
}

// The kept frame's texture, the back buffer's size and format.
bool make_kept(ID3D12Device* device, const D3D12_RESOURCE_DESC& frame) {
  if (kept && same_frame(kept->GetDesc(), frame)) {
    return true;
  }
  retire(kept);
  kept_filled = false;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = frame;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&kept)));
}

// The buffer the picture's pixels are read back through, rows as the GPU lays them out.
bool make_readback(ID3D12Device* device, const D3D12_RESOURCE_DESC& frame) {
  if (readback && readback_format == frame.Format &&
      footprint.Footprint.Width == UINT(frame.Width) &&
      footprint.Footprint.Height == frame.Height) {
    return true;
  }
  retire(readback);
  UINT64 bytes = 0;
  device->GetCopyableFootprints(&frame, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  readback_format = frame.Format;
  return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&readback)));
}

}  // namespace

void on_present(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* back) {
  const Phase now = phase.load();
  if (now == Phase::Idle) {
    reading_at = 0;
  }
  if (!device || !queue || !back || !ready_for(device)) {
    return;
  }
  const UINT64 completed = fence->GetCompletedValue();
  release_retired(completed);
  if (now == Phase::Idle) {
    return;
  }
  const D3D12_RESOURCE_DESC frame = back->GetDesc();
  if (now == Phase::Saving) {
    if (!make_kept(device, frame)) {
      kept.Reset();
    }
    List* list = free_list(device, completed);
    if (!list) {
      return;
    }
    ID3D12GraphicsCommandList* commands = list->commands.Get();
    if (kept) {
      if (kept_filled) {
        transition(commands, kept.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
      }
      transition(commands, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      commands->CopyResource(kept.Get(), back);
      transition(commands, back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
      transition(commands, kept.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
      kept_filled = true;
    }
    submit(queue, *list);
    presents = 0;
    taken = Picture();
    phase = Phase::Saved;
    return;
  }
  // Frames are counted from the camera's start (Saved only shows the kept frame).
  if (now != Phase::Saved) {
    presents += 1;
  }
  // The picture's pixels, once the GPU finished their copy. The picture goes to the scripts at
  // once (it is encoded meanwhile); the kept frame is shown on while the game's view comes back.
  if (now == Phase::Filming && reading_at != 0 && completed >= reading_at) {
    reading_at = 0;
    read_pixels();
    taken.id = filming_id.load();
    taken.scaled_width = filming_width.load();
    deliver_taken();
    presents = 0;
    phase = Phase::Filmed;
  }
  if (now == Phase::Restoring && presents >= kRestorePresents) {
    phase = Phase::Idle;
  }
  bool copy_pixels =
      now == Phase::Filming && presents >= kFilmPresents && phase.load() == Phase::Filming;
  if (copy_pixels && reading_at != 0) {
    // Asked already; its copy is still on the GPU.
    copy_pixels = false;
  }
  if (copy_pixels && !make_readback(device, frame)) {
    copy_pixels = false;
    readback.Reset();
    taken.id = filming_id.load();
    taken.failure = "the frame could not be copied";
    deliver_taken();
    presents = 0;
    phase = Phase::Filmed;
  }
  const bool show_kept = kept && kept_filled;
  if (!copy_pixels && !show_kept) {
    return;
  }
  List* list = free_list(device, completed);
  if (!list) {
    return;
  }
  ID3D12GraphicsCommandList* commands = list->commands.Get();
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT;
  if (copy_pixels) {
    transition(commands, back, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = back;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  // The player sees the kept frame while the camera is elsewhere.
  if (show_kept) {
    transition(commands, back, state, D3D12_RESOURCE_STATE_COPY_DEST);
    state = D3D12_RESOURCE_STATE_COPY_DEST;
    commands->CopyResource(back, kept.Get());
  }
  transition(commands, back, state, D3D12_RESOURCE_STATE_PRESENT);
  submit(queue, *list);
  if (copy_pixels) {
    reading_at = signalled;
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
