#include "world_textures.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>

#include "MinHook.h"
#include "world_texture_match.hpp"

namespace kcds::runtime::render::world_textures {
namespace {

using Microsoft::WRL::ComPtr;

// The device's resource makers in its table of functions (d3d12.h): CreateCommittedResource and
// CreatePlacedResource of ID3D12Device, CreateCommittedResource1 of ID3D12Device4,
// CreateCommittedResource2 and CreatePlacedResource1 of ID3D12Device8.
constexpr std::size_t kCommittedSlot = 27;
constexpr std::size_t kPlacedSlot = 29;
constexpr std::size_t kCommitted1Slot = 53;
constexpr std::size_t kCommitted2Slot = 69;
constexpr std::size_t kPlaced1Slot = 70;
constexpr std::size_t kWatchedMax = 8;

using Committed = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                              D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                              D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                              REFIID, void**);
using Placed = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
                                           const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                           const D3D12_CLEAR_VALUE*, REFIID, void**);
using Committed1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                               D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                               D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                               ID3D12ProtectedResourceSession*, REFIID, void**);
// D3D12_RESOURCE_DESC1 begins with the fields of D3D12_RESOURCE_DESC.
using Committed2 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                               D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*,
                                               D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                               ID3D12ProtectedResourceSession*, REFIID, void**);
using Placed1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
                                            const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES,
                                            const D3D12_CLEAR_VALUE*, REFIID, void**);

Logger log_line = nullptr;
bool watching = false;
Committed original_committed = nullptr;
Placed original_placed = nullptr;
Committed1 original_committed1 = nullptr;
Committed2 original_committed2 = nullptr;
Placed1 original_placed1 = nullptr;

std::mutex textures_mutex;
std::array<Size, kWatchedMax> watched{};
std::size_t watched_count = 0;
// (width << 32 | height) -> the latest screen texture of that size.
std::map<std::uint64_t, ComPtr<ID3D12Resource>> textures;

std::uint64_t key_of(std::uint64_t width, std::uint32_t height) {
  return (width << 32) | height;
}

bool screen_texture(const D3D12_RESOURCE_DESC& desc) {
  // Most of what the game makes is told apart before the lock.
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Flags != 0) {
    return false;
  }
  TextureAsk ask;
  ask.dimension = static_cast<std::uint32_t>(desc.Dimension);
  ask.width = desc.Width;
  ask.height = desc.Height;
  ask.depth_or_array = desc.DepthOrArraySize;
  ask.mips = desc.MipLevels;
  ask.format = static_cast<std::uint32_t>(desc.Format);
  ask.samples = desc.SampleDesc.Count;
  ask.flags = static_cast<std::uint32_t>(desc.Flags);
  std::lock_guard<std::mutex> lock(textures_mutex);
  return is_screen_texture(ask, watched.data(), watched_count);
}

void keep(void** made, const D3D12_RESOURCE_DESC& desc, const char* asked_as) {
  ComPtr<ID3D12Resource> resource;
  if (!made || !*made ||
      FAILED(static_cast<IUnknown*>(*made)->QueryInterface(IID_PPV_ARGS(&resource)))) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(textures_mutex);
    textures[key_of(desc.Width, desc.Height)] = resource;
  }
  if (log_line) {
    log_line("RENDER world screen texture %llux%u format=%u mips=%u made (asked as %s)",
             static_cast<unsigned long long>(desc.Width), desc.Height,
             static_cast<unsigned>(desc.Format), static_cast<unsigned>(desc.MipLevels), asked_as);
  }
}

// The screen's texture on memory of its own, open to every queue at once: the page's copies need
// no knowledge of the state the game keeps it in. With mip levels it can also be drawn into, so
// they are made again from each page frame. Fails as the game's own call would.
HRESULT make_screen_texture(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                            D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                            REFIID riid, void** made, const char* asked_as) {
  D3D12_RESOURCE_DESC shared = desc;
  shared.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
  if (desc.MipLevels > 1) {
    shared.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  const HRESULT result =
      original_committed(device, &heap, D3D12_HEAP_FLAG_NONE, &shared, state, clear, riid, made);
  if (SUCCEEDED(result)) {
    keep(made, desc, asked_as);
  } else if (log_line) {
    log_line("RENDER world screen texture %llux%u could not be made: 0x%lx",
             static_cast<unsigned long long>(desc.Width), desc.Height, result);
  }
  return result;
}

HRESULT STDMETHODCALLTYPE hooked_committed(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heap,
                                           D3D12_HEAP_FLAGS heap_flags,
                                           const D3D12_RESOURCE_DESC* desc,
                                           D3D12_RESOURCE_STATES state,
                                           const D3D12_CLEAR_VALUE* clear, REFIID riid,
                                           void** made) {
  if (desc && made && screen_texture(*desc)) {
    const HRESULT result =
        make_screen_texture(device, *desc, state, clear, riid, made, "committed");
    if (SUCCEEDED(result)) {
      return result;
    }
  }
  return original_committed(device, heap, heap_flags, desc, state, clear, riid, made);
}

HRESULT STDMETHODCALLTYPE hooked_placed(ID3D12Device* device, ID3D12Heap* heap, UINT64 offset,
                                        const D3D12_RESOURCE_DESC* desc,
                                        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                        REFIID riid, void** made) {
  // The place the game's heap set aside for the texture stays unused.
  if (desc && made && screen_texture(*desc)) {
    const HRESULT result = make_screen_texture(device, *desc, state, clear, riid, made, "placed");
    if (SUCCEEDED(result)) {
      return result;
    }
  }
  return original_placed(device, heap, offset, desc, state, clear, riid, made);
}

HRESULT STDMETHODCALLTYPE hooked_committed1(
    ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
    ID3D12ProtectedResourceSession* session, REFIID riid, void** made) {
  if (!session && desc && made && screen_texture(*desc)) {
    const HRESULT result =
        make_screen_texture(device, *desc, state, clear, riid, made, "committed1");
    if (SUCCEEDED(result)) {
      return result;
    }
  }
  return original_committed1(device, heap, heap_flags, desc, state, clear, session, riid, made);
}

HRESULT STDMETHODCALLTYPE hooked_committed2(
    ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
    ID3D12ProtectedResourceSession* session, REFIID riid, void** made) {
  const auto* plain = reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc);
  if (!session && desc && made && screen_texture(*plain)) {
    const HRESULT result =
        make_screen_texture(device, *plain, state, clear, riid, made, "committed2");
    if (SUCCEEDED(result)) {
      return result;
    }
  }
  return original_committed2(device, heap, heap_flags, desc, state, clear, session, riid, made);
}

HRESULT STDMETHODCALLTYPE hooked_placed1(ID3D12Device* device, ID3D12Heap* heap, UINT64 offset,
                                         const D3D12_RESOURCE_DESC1* desc,
                                         D3D12_RESOURCE_STATES state,
                                         const D3D12_CLEAR_VALUE* clear, REFIID riid, void** made) {
  const auto* plain = reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc);
  if (desc && made && screen_texture(*plain)) {
    const HRESULT result = make_screen_texture(device, *plain, state, clear, riid, made, "placed1");
    if (SUCCEEDED(result)) {
      return result;
    }
  }
  return original_placed1(device, heap, offset, desc, state, clear, riid, made);
}

template <typename Function>
bool hook_slot(void** table, std::size_t slot, Function hooked, Function* original) {
  void* target = table[slot];
  if (MH_CreateHook(target, reinterpret_cast<void*>(hooked), reinterpret_cast<void**>(original)) !=
      MH_OK) {
    return false;
  }
  return MH_EnableHook(target) == MH_OK;
}

}  // namespace

void watch(ID3D12Device* device, Logger logger) {
  if (watching || !device) {
    return;
  }
  watching = true;
  log_line = logger;
  void** table = *reinterpret_cast<void***>(device);
  const bool base = hook_slot(table, kCommittedSlot, &hooked_committed, &original_committed) &&
                    hook_slot(table, kPlacedSlot, &hooked_placed, &original_placed);
  bool newer = true;
  ComPtr<ID3D12Device4> device4;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4)))) {
    void** table4 = *reinterpret_cast<void***>(device4.Get());
    newer = hook_slot(table4, kCommitted1Slot, &hooked_committed1, &original_committed1);
  }
  ComPtr<ID3D12Device8> device8;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device8)))) {
    void** table8 = *reinterpret_cast<void***>(device8.Get());
    newer = hook_slot(table8, kCommitted2Slot, &hooked_committed2, &original_committed2) &&
            hook_slot(table8, kPlaced1Slot, &hooked_placed1, &original_placed1) && newer;
  }
  if (log_line) {
    log_line("RENDER world screen textures %s", base && newer ? "watched" : "partly watched");
  }
}

void want(int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(textures_mutex);
  for (std::size_t at = 0; at < watched_count; ++at) {
    if (watched[at].width == width && watched[at].height == height) {
      return;
    }
  }
  if (watched_count < kWatchedMax) {
    watched[watched_count] = Size{width, height};
    watched_count += 1;
  }
}

bool latest(int width, int height, ID3D12Resource** out) {
  if (!out || width <= 0 || height <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(textures_mutex);
  const auto found =
      textures.find(key_of(static_cast<std::uint64_t>(width), static_cast<std::uint32_t>(height)));
  if (found == textures.end()) {
    return false;
  }
  found->second.CopyTo(out);
  return true;
}

}  // namespace kcds::runtime::render::world_textures
