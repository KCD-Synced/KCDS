#include "skinning_metrics.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "MinHook.h"
#include "kcd2s/bridge/sigscan.hpp"
#include "skinning_metrics_sample.hpp"

namespace kcds::runtime::characters::skinning::metrics {
namespace {

namespace bridge = kcd2s::bridge;

// Live, WHGame 1.5.5: upload and buffer Unmap. All five
// signatures have one executable-section match in SHA256
// bdf8f9e4a11257a72b64c84700e284c29e4c4ccaf5b8d4bfa7d0b2a7294479f7.
// Native addresses are evidence only; installation resolves and validates signatures.
constexpr const char* kUploadPattern =
    "48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 68 "
    "48 8B 82 60 8D 00 00 48 8B F1 48 8B FA 48 8B 58 58 48 8B CB E8 ?? ?? ?? ?? "
    "48 8B C8 48 89 45 C0 8B 40 60 89 45 50 E8 ?? ?? ?? ?? 48 8D 8B 28 01 00 00 "
    "4C 8B E8 8B 41 60 89 45 58 48 89 4D C8 E8";
constexpr const char* kUnmapPattern =
    "48 83 EC 48 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 30 8B C2 4C 8D 44 24 20 "
    "C6 41 74 00 33 D2 48 89 44 24 28 48 8B 41 58 48 C7 44 24 20 00 00 00 00 "
    "48 8B 48 08 48 8B 49 70 48 8B 01 FF 50 48";
constexpr const char* kCallsPattern =
    "48 8B 4D C0 41 C1 E6 05 41 8B D6 E8 ?? ?? ?? ?? 48 8B 4D C8 "
    "42 8D 14 FD 00 00 00 00 E8 ?? ?? ?? ?? 33 C0 45 33 F6 F0 44 0F B1 B6 84 36 06 00";
constexpr const char* kFramePattern =
    "48 63 8F 40 8D 00 00 4C 8D A6 48 36 06 00 48 B8 AB AA AA AA AA AA AA AA "
    "45 8B FE 48 F7 E1";
constexpr const char* kCountsPattern =
    "42 8B 9C 86 78 36 06 00 42 8B 8C 8E 78 36 06 00 03 D9 48 8D 4D 48 E8";
constexpr std::size_t kViewFrameOffset = 36160;
constexpr std::size_t kRequestCountsOffset = 407160;
constexpr ULONGLONG kReportMilliseconds = 5000;

using Upload = void*(__fastcall*)(void*, void*);
using Unmap = std::uintptr_t(__fastcall*)(void*, std::uint32_t);
Upload original_upload = nullptr;
Unmap original_unmap = nullptr;
const void* bone_return = nullptr;
const void* morph_return = nullptr;
std::uint8_t* module_image = nullptr;
std::size_t module_size = 0;
Logger note = nullptr;
std::mutex install_mutex;
bool attempted = false;
bool installed = false;

struct UploadContext {
  UploadSample sample;
  void* renderer = nullptr;
};
thread_local UploadContext* current_upload = nullptr;
SRWLOCK samples_lock = SRWLOCK_INIT;
Window window;
ULONGLONG last_report = 0;
std::atomic<std::uint64_t> dropped_samples{0};

bool enabled(const char* name) {
  char value[4]{};
  return GetEnvironmentVariableA(name, value, sizeof(value)) == 1 && value[0] == '1';
}

bool in_image(const void* pointer, std::size_t size) {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  const auto base = reinterpret_cast<std::uintptr_t>(module_image);
  return address >= base && address - base <= module_size && size <= module_size - (address - base);
}

bool readable(const void* pointer, std::size_t size) {
  auto cursor = reinterpret_cast<std::uintptr_t>(pointer);
  if (!pointer || size > UINTPTR_MAX - cursor) {
    return false;
  }
  const auto end = cursor + size;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
      return false;
    }
    const auto protection = region.Protect & 0xFF;
    if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
        protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
      return false;
    }
    const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (next <= cursor) {
      return false;
    }
    cursor = next < end ? next : end;
  }
  return true;
}

const IMAGE_NT_HEADERS64* headers() {
  if (!in_image(module_image, sizeof(IMAGE_DOS_HEADER)) ||
      !readable(module_image, sizeof(IMAGE_DOS_HEADER))) {
    return nullptr;
  }
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
      static_cast<std::size_t>(dos->e_lfanew) > module_size) {
    return nullptr;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module_image + dos->e_lfanew);
  if (!in_image(nt, sizeof(*nt)) || !readable(nt, sizeof(*nt)) ||
      nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
      nt->OptionalHeader.SizeOfImage != module_size || nt->FileHeader.NumberOfSections == 0) {
    return nullptr;
  }
  const auto bytes = sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections;
  return in_image(IMAGE_FIRST_SECTION(nt), bytes) && readable(IMAGE_FIRST_SECTION(nt), bytes)
             ? nt
             : nullptr;
}

std::uint8_t* resolve_unique(const char* pattern, const IMAGE_NT_HEADERS64* nt) {
  const auto signature = bridge::Signature::parse(pattern);
  if (!signature) {
    return nullptr;
  }
  std::uint8_t* match = nullptr;
  const auto* sections = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto& section = sections[i];
    if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
      continue;
    }
    const auto size = std::size_t{section.Misc.VirtualSize};
    if (section.VirtualAddress > module_size || size > module_size - section.VirtualAddress) {
      return nullptr;
    }
    auto* begin = module_image + section.VirtualAddress;
    if (!readable(begin, size)) {
      return nullptr;
    }
    std::size_t cursor = 0;
    while (cursor < size) {
      const auto offset = bridge::scan(begin + cursor, size - cursor, *signature);
      if (offset == bridge::knpos) {
        break;
      }
      if (match) {
        return nullptr;
      }
      match = begin + cursor + offset;
      cursor += offset + 1;
    }
  }
  return match;
}

const void* call_target(const std::uint8_t* call) {
  if (!in_image(call, 5) || call[0] != 0xE8) {
    return nullptr;
  }
  const auto offset = static_cast<std::size_t>(call - module_image);
  const auto target = bridge::rip_target(module_image, module_size, offset + 1, offset + 5);
  return target == bridge::knpos ? nullptr : module_image + target;
}

// These guards cover diagnostic reads only. Never catch an exception from the native upload
// or Unmap and pretend that its work completed. The original controls its own failure behavior.
bool read_u32(const void* object, std::size_t offset, std::uint32_t& value) {
  if (!object || offset > UINTPTR_MAX - reinterpret_cast<std::uintptr_t>(object)) {
    return false;
  }
  const auto address = reinterpret_cast<std::uintptr_t>(object) + offset;
  __try {
    value = *reinterpret_cast<const volatile std::uint32_t*>(address);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool sample_requests(const UploadContext& context, std::uint32_t& value) {
  if (!context.sample.frame_valid || !context.renderer) {
    return false;
  }
  const auto base = reinterpret_cast<std::uintptr_t>(context.renderer);
  const auto offset = kRequestCountsOffset + sizeof(LONG) * (context.sample.frame % 3u);
  if (offset > UINTPTR_MAX - base || (base + offset) % alignof(LONG) != 0) {
    return false;
  }
  __try {
    // Read a single aligned counter while the original upload still owns its renderer argument.
    // This does not acquire a list lock or pin a request pool; no request pointer is ever read.
    // Concurrent insertion/reset can change the count, so it is explicitly only a snapshot.
    const auto count =
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(base + offset), 0, 0);
    if (count < 0 || static_cast<std::uint32_t>(count) > kMaximumRequestSample) {
      return false;
    }
    value = static_cast<std::uint32_t>(count);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void publish(const UploadSample& sample) {
  if (!TryAcquireSRWLockExclusive(&samples_lock)) {
    dropped_samples.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  window.add(sample);
  const auto now = GetTickCount64();
  if (!last_report) {
    last_report = now;
  }
  const auto elapsed = now - last_report;
  Totals report{};
  const bool ready = elapsed >= kReportMilliseconds;
  if (ready) {
    report = window.totals;
    window.reset();
    last_report = now;
  }
  ReleaseSRWLockExclusive(&samples_lock);
  if (!ready) {
    return;
  }
  const auto dropped = dropped_samples.exchange(0, std::memory_order_relaxed);
  note(
      "SKINNING metrics interval_ms=%llu uploads=%llu complete=%llu incomplete=%llu "
      "frames=%llu frame_table_full=%llu dropped=%llu requests_sample_peak=%u "
      "request_sample_missing=%llu accepted_bones_peak=%u accepted_morphs_peak=%u "
      "bone_used_peak=%u morph_used_peak=%u bone_capacity_min=%u bone_capacity_max=%u "
      "morph_capacity_min=%u morph_capacity_max=%u bone_utilization_peak_pct=%.2f "
      "morph_utilization_peak_pct=%.2f reserved=768 demand=unavailable overflow=unavailable",
      static_cast<unsigned long long>(elapsed), static_cast<unsigned long long>(report.uploads),
      static_cast<unsigned long long>(report.complete),
      static_cast<unsigned long long>(report.incomplete),
      static_cast<unsigned long long>(report.frames),
      static_cast<unsigned long long>(report.frame_table_full),
      static_cast<unsigned long long>(dropped), report.requests_sample_peak,
      static_cast<unsigned long long>(report.request_sample_missing), report.accepted_bones_peak,
      report.accepted_morphs_peak, report.bone_used_peak, report.morph_used_peak,
      report.bone_capacity_min, report.bone_capacity_max, report.morph_capacity_min,
      report.morph_capacity_max, report.bone_utilization_ppm / 10000.0,
      report.morph_utilization_ppm / 10000.0);
}

std::uintptr_t __fastcall observe_unmap(void* buffer, std::uint32_t bytes) {
  auto* context = current_upload;
  const auto* caller = _ReturnAddress();
  const bool bones = context && caller == bone_return;
  const bool morphs = context && caller == morph_return;
  BufferSample sample;
  if (bones || morphs) {
    std::uint32_t capacity = 0;
    std::uint32_t stride = 0;
    if (read_u32(buffer, 96, capacity) && read_u32(buffer, 100, stride)) {
      sample = decode_buffer(bytes, capacity, stride, bones ? 32u : 8u);
    }
  }

  const auto result = original_unmap(buffer, bytes);
  if (bones) {
    context->sample.duplicate |= context->sample.bone_calls++ != 0;
    context->sample.bones = sample;
  } else if (morphs) {
    context->sample.duplicate |= context->sample.morph_calls++ != 0;
    context->sample.morphs = sample;
    context->sample.requests_valid = sample_requests(*context, context->sample.requests);
  }
  return result;
}

void* __fastcall observe_upload(void* renderer, void* view) {
  UploadContext context;
  context.renderer = renderer;
  context.sample.renderer_key = reinterpret_cast<std::uintptr_t>(renderer);
  context.sample.frame_valid =
      read_u32(view, kViewFrameOffset, context.sample.frame) && context.sample.frame <= INT32_MAX;
  auto* previous = current_upload;
  current_upload = &context;
  void* result = nullptr;
  __try {
    // The trampoline is called exactly once. Both observations happen inside its native calls;
    // after it returns only our copied scalars are inspected, never native request storage.
    result = original_upload(renderer, view);
  } __finally {
    current_upload = previous;
  }
  publish(context.sample);
  return result;
}

}  // namespace

bool install(std::uint8_t* image, std::size_t image_size, Logger logger) {
  if (!enabled("KCDS_TEST_HOOKS") || !enabled("KCDS_TEST_CHARACTER_METRICS") || !logger) {
    return false;
  }
  std::lock_guard<std::mutex> lock(install_mutex);
  if (attempted) {
    return installed;
  }
  attempted = true;
  module_image = image;
  module_size = image_size;
  note = logger;
  const auto* nt = headers();
  if (!nt) {
    note("SKINNING metrics invalid module image; disabled");
    return false;
  }
  auto* upload = resolve_unique(kUploadPattern, nt);
  auto* unmap = resolve_unique(kUnmapPattern, nt);
  auto* calls = resolve_unique(kCallsPattern, nt);
  auto* frame = resolve_unique(kFramePattern, nt);
  auto* counts = resolve_unique(kCountsPattern, nt);
  if (!upload || !unmap || !calls || !frame || !counts || calls != upload + 0x1A0 ||
      frame != upload + 0xB9 || counts != upload + 0x1F0 || call_target(calls + 11) != unmap ||
      call_target(calls + 28) != unmap) {
    note("SKINNING metrics signatures or native call relationships changed; disabled");
    return false;
  }
  bone_return = calls + 16;
  morph_return = calls + 33;
  const auto initialized = MH_Initialize();
  if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
    note("SKINNING metrics MinHook initialization failed=%d", static_cast<int>(initialized));
    return false;
  }
  auto result = MH_CreateHook(unmap, reinterpret_cast<void*>(observe_unmap),
                              reinterpret_cast<void**>(&original_unmap));
  if (result != MH_OK) {
    note("SKINNING metrics Unmap hook creation failed=%d", static_cast<int>(result));
    return false;
  }
  result = MH_CreateHook(upload, reinterpret_cast<void*>(observe_upload),
                         reinterpret_cast<void**>(&original_upload));
  if (result != MH_OK) {
    MH_RemoveHook(unmap);
    note("SKINNING metrics upload hook creation failed=%d", static_cast<int>(result));
    return false;
  }
  result = MH_EnableHook(unmap);
  if (result == MH_OK) {
    result = MH_EnableHook(upload);
  }
  if (result != MH_OK) {
    // A surviving Unmap hook is a pass-through because no upload TLS scope is active. Keep its
    // trampoline pointer valid if MinHook cannot remove it during rollback.
    const auto upload_rollback = MH_RemoveHook(upload);
    const auto unmap_rollback = MH_RemoveHook(unmap);
    note("SKINNING metrics hook enable failed=%d rollback_upload=%d rollback_unmap=%d",
         static_cast<int>(result), static_cast<int>(upload_rollback),
         static_cast<int>(unmap_rollback));
    return false;
  }
  installed = true;
  note(
      "SKINNING metrics enabled for tests; exact accepted upload records, sampled requests; "
      "demand, overflow, and rendered character count unavailable");
  return true;
}

}  // namespace kcds::runtime::characters::skinning::metrics
