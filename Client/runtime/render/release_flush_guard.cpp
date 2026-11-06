#include "release_flush_guard.hpp"

#include <cstring>
#include <vector>

#include "MinHook.h"

namespace kcds::runtime::render::release_flush_guard {
namespace {

// The list flush and the release enqueue, by their place in WHGame 1.5.5
// and checked by their first bytes.
constexpr std::size_t kFlushOffset = 0x1DEA930;
constexpr std::size_t kEnqueueOffset = 0x7ABC50;
constexpr std::uint8_t kFlushPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                           0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56,
                                           0x41, 0x57, 0x48, 0x83, 0xEC, 0x60};
constexpr std::uint8_t kEnqueuePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89,
                                             0x74, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24,
                                             0x10, 0x57, 0x48, 0x83, 0xEC, 0x30};

// The flush: the renderer's address (held), the list (0 or 1).
using Flush = void(__fastcall*)(void** renderer, unsigned list);
// The enqueue: an unused first argument, the resource.
using Enqueue = void(__fastcall*)(void* unused, void* resource);

struct Kept {
  void* unused;
  void* resource;
};

Logger log_line = nullptr;
Flush original_flush = nullptr;
Enqueue original_enqueue = nullptr;

// Per thread: how deep in a flush this thread is, and the releases it handed in meanwhile.
thread_local int flushing = 0;
thread_local std::vector<Kept> kept;
thread_local bool told = false;

void __fastcall guarded_enqueue(void* unused, void* resource) {
  if (flushing > 0) {
    kept.push_back({unused, resource});
    return;
  }
  original_enqueue(unused, resource);
}

void __fastcall guarded_flush(void** renderer, unsigned list) {
  ++flushing;
  original_flush(renderer, list);
  --flushing;
  if (flushing > 0 || kept.empty()) {
    return;
  }
  std::vector<Kept> releases;
  releases.swap(kept);
  if (log_line && !told) {
    told = true;
    log_line("RENDER release flush handed on %zu release(s) after the list lock", releases.size());
  }
  for (const Kept& release : releases) {
    original_enqueue(release.unused, release.resource);
  }
}

bool matches(std::uint8_t* image, std::size_t image_size, std::size_t offset,
             const std::uint8_t* prologue, std::size_t length) {
  if (offset + length > image_size) {
    return false;
  }
  return std::memcmp(image + offset, prologue, length) == 0;
}

}  // namespace

void install(std::uint8_t* image, std::size_t image_size, Logger logger) {
  log_line = logger;
  if (!image || original_flush) {
    return;
  }
  const bool found =
      matches(image, image_size, kFlushOffset, kFlushPrologue, sizeof(kFlushPrologue)) &&
      matches(image, image_size, kEnqueueOffset, kEnqueuePrologue, sizeof(kEnqueuePrologue));
  if (!found) {
    if (log_line) {
      log_line("RENDER release flush not where WHGame 1.5.5 has it: no guard");
    }
    return;
  }
  void* flush = image + kFlushOffset;
  void* enqueue = image + kEnqueueOffset;
  const bool hooked = MH_CreateHook(enqueue, reinterpret_cast<void*>(&guarded_enqueue),
                                    reinterpret_cast<void**>(&original_enqueue)) == MH_OK &&
                      MH_CreateHook(flush, reinterpret_cast<void*>(&guarded_flush),
                                    reinterpret_cast<void**>(&original_flush)) == MH_OK &&
                      MH_EnableHook(enqueue) == MH_OK && MH_EnableHook(flush) == MH_OK;
  if (log_line) {
    log_line("RENDER release flush guard hooked=%u", unsigned(hooked));
  }
}

}  // namespace kcds::runtime::render::release_flush_guard
