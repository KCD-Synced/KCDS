#include "material_capacity.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>

#include "MinHook.h"
#include "kcd2s/bridge/sigscan.hpp"
#include "material_capacity_plan.hpp"

namespace kcds::runtime::characters::materials {
namespace {

namespace bridge = kcd2s::bridge;

// WHGame 1.5.5, IDA2: the class-specific clothing manager constructor,
// its sole allocation caller, and CMaterialBufferStageWH::Init.
// Each pattern is unique in executable sections of SHA256
// bdf8f9e4a11257a72b64c84700e284c29e4c4ccaf5b8d4bfa7d0b2a7294479f7.
// No fixed address resolves a native. The constructor's specialization owns 128-byte entries;
// the separate feature manager and generic allocation/vector functions are never hooked.
constexpr char kPoolConstructor[] =
    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57 48 83 EC 20 "
    "48 8D 05 ?? ?? ?? ?? 44 8B F2 48 89 01 4C 8B F9";
constexpr char kPoolCaller[] = "41 8B 16 4C 8B C6 48 8B C8 E8 56 01 00 00 48 8B D8 48 8B 74 24 40";
constexpr char kPoolConstructorTail[] =
    "41 0F 11 8F 80 00 00 00 E8 ?? ?? ?? ?? 49 8B CF 66 45 89 B7 D0 00 00 00 E8 ?? ?? ?? ?? "
    "48 8B 5C 24 40 49 8B C7";
constexpr char kPoolsInitialize[] =
    "48 89 5C 24 08 55 48 8D 6C 24 A9 48 81 EC C0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
    "48 89 45 47 48 8B 05 ?? ?? ?? ?? 4C 8D 45 C7 48 8B 1D ?? ?? ?? ?? 48 8D 55 B7 0F 57 C0 "
    "0F 57 C9 0F 11 45 C7 8B 88 88 0A 00 00";
constexpr char kMaterialManagerConstructor[] =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F9 48 89 01 "
    "33 F6 48 89 71 08 48 83 C1 08 FF 15";
constexpr char kStageInitialize[] =
    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 40 48 8B E9 "
    "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 28 06 00 00 48 8B C8 4C 8B F0 48 8B 10 FF 92 00 "
    "01 00 00 48 8B C8 48 8B F0 48 8B 10 FF 52 08";
constexpr char kStageConstructor[] =
    "48 8D 4B 30 48 C7 40 28 00 00 00 00 48 8D 05 ?? ?? ?? ?? 48 89 03 E8 ?? ?? ?? ?? 48 8D "
    "8B A8 00 00 00 E8 ?? ?? ?? ??";
constexpr char kScaleVariable[] = "KCDS_MATERIAL_CAPACITY_SCALE";
constexpr std::size_t kPoolSize = 216;
constexpr std::size_t kOwnerSize = 280;
constexpr std::size_t kStageSize = 288;
constexpr std::size_t kClothingBuffer = 48;

using ConstructPool = void*(__fastcall*)(void*, std::uint32_t, const void*);
using InitializeStage = void(__fastcall*)(void*);
ConstructPool original_construct = nullptr;
InitializeStage original_stage = nullptr;
std::uint8_t* module_image = nullptr;
std::size_t module_size = 0;
const void* pool_vtable = nullptr;
const void* owner_vtable = nullptr;
const void* stage_vtable = nullptr;
const void* constructor_return = nullptr;
const void* const* owner_slot = nullptr;
Logger note = nullptr;
unsigned configured_scale = kDefaultScale;
std::mutex install_mutex;
bool attempted = false;
std::atomic<bool> installed{false};
std::atomic<std::uint32_t> observed_constructions{0};
std::atomic<std::uint32_t> increased_constructions{0};
std::atomic<std::uint32_t> skipped_constructions{0};
std::atomic<std::uint32_t> verified_gpu_stages{0};
std::atomic<std::uint32_t> last_cpu_count{0};
std::atomic<std::uint32_t> last_gpu_count{0};

bool in_image(const void* pointer, std::size_t size) {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  const auto base = reinterpret_cast<std::uintptr_t>(module_image);
  return address >= base && address - base <= module_size && size <= module_size - (address - base);
}

bool accessible(const void* pointer, std::size_t size, bool writable = false) {
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
    const bool can_write = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                           protection == PAGE_EXECUTE_READWRITE ||
                           protection == PAGE_EXECUTE_WRITECOPY;
    const bool can_read =
        can_write || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
    if (!can_read || (writable && !can_write)) {
      return false;
    }
    const auto region_end =
        reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (region_end <= cursor) {
      return false;
    }
    cursor = region_end < end ? region_end : end;
  }
  return true;
}

template <typename T>
T field(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

const IMAGE_NT_HEADERS64* headers() {
  if (!in_image(module_image, sizeof(IMAGE_DOS_HEADER)) ||
      !accessible(module_image, sizeof(IMAGE_DOS_HEADER))) {
    return nullptr;
  }
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
      static_cast<std::size_t>(dos->e_lfanew) > module_size) {
    return nullptr;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module_image + dos->e_lfanew);
  if (!in_image(nt, sizeof(*nt)) || !accessible(nt, sizeof(*nt)) ||
      nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
      nt->OptionalHeader.SizeOfImage != module_size || nt->FileHeader.NumberOfSections == 0) {
    return nullptr;
  }
  const auto bytes = sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections;
  if (!in_image(IMAGE_FIRST_SECTION(nt), bytes) || !accessible(IMAGE_FIRST_SECTION(nt), bytes)) {
    return nullptr;
  }
  return nt;
}

std::uint8_t* resolve_unique(const char* name, const char* pattern, const IMAGE_NT_HEADERS64* nt) {
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
    if (!accessible(begin, size)) {
      return nullptr;
    }
    std::size_t cursor = 0;
    while (cursor < size) {
      const auto offset = bridge::scan(begin + cursor, size - cursor, *signature);
      if (offset == bridge::knpos) {
        break;
      }
      if (match) {
        note("MATERIAL capacity ambiguous signature=%s; native allocations unchanged", name);
        return nullptr;
      }
      match = begin + cursor + offset;
      cursor += offset + 1;
    }
  }
  return match;
}

std::uint8_t* relative(const std::uint8_t* instruction, std::size_t operand, std::size_t length) {
  if (!in_image(instruction, length) || operand + sizeof(std::int32_t) > length) {
    return nullptr;
  }
  const auto offset = static_cast<std::size_t>(instruction - module_image);
  const auto target =
      bridge::rip_target(module_image, module_size, offset + operand, offset + length);
  return target < module_size ? module_image + target : nullptr;
}

const void* current_owner() {
  if (!owner_slot || !accessible(owner_slot, sizeof(*owner_slot))) {
    return nullptr;
  }
  const auto* owner = field<const void*>(owner_slot, 0);
  return accessible(owner, kOwnerSize) && field<const void*>(owner, 0) == owner_vtable ? owner
                                                                                       : nullptr;
}

const void* current_pool(const void* owner) {
  if (!owner) {
    return nullptr;
  }
  const auto* pool = field<const void*>(owner, 192);
  return accessible(pool, kPoolSize) && field<const void*>(pool, 0) == pool_vtable ? pool : nullptr;
}

bool valid_cpu(const void* pool, std::uint32_t expected) {
  return accessible(pool, kPoolSize) && field<const void*>(pool, 0) == pool_vtable &&
         matches_cpu_allocation(expected, field<std::uint16_t>(pool, 208),
                                field<std::uintptr_t>(pool, 144), field<std::uintptr_t>(pool, 152),
                                field<std::uintptr_t>(pool, 160));
}

void* __fastcall construct_pool(void* storage, std::uint32_t native_count, const void* defaults) {
  const auto* caller = _ReturnAddress();
  const auto* owner = current_owner();
  const auto plan = make_plan(configured_scale, native_count);
  // The target is a constructor, so its storage is uninitialized and must not be interpreted as
  // a previous instance. Instead require the exact native factory and its unpublished owner slot.
  const bool fresh = caller == constructor_return && owner &&
                     field<const void*>(owner, 192) == nullptr &&
                     accessible(storage, kPoolSize, true) && accessible(defaults, kElementBytes);
  const auto effective = fresh && plan.valid() ? plan.effective : native_count;
  if (!fresh || !plan.valid()) {
    ++skipped_constructions;
    note("MATERIAL capacity constructor scope/count rejected count=%u; forwarding unchanged",
         native_count);
  } else {
    note("MATERIAL capacity pre-init scale=%u count=%u->%u bytes=%llu", configured_scale,
         native_count, effective, static_cast<unsigned long long>(allocation_bytes(effective)));
  }

  // Preserve the original allocation/default initialization/failure semantics exactly once.
  // No cvar, existing object, shared material, or GPU resource is modified here.
  void* result = original_construct(storage, effective, defaults);
  ++observed_constructions;
  const bool matched =
      fresh && plan.valid() && result == storage && valid_cpu(result, effective) &&
      field<std::uint16_t>(result, 210) == 1 && field<std::uint64_t>(result, 200) == 0 &&
      std::memcmp(static_cast<const std::uint8_t*>(result) + 16, defaults, kElementBytes) == 0;
  if (matched) {
    last_cpu_count.store(effective);
    if (plan.result == PlanResult::increase) {
      ++increased_constructions;
    }
  }
  note("MATERIAL capacity CPU constructor=%p count=%u result=%s; GPU allocation pending", result,
       effective, matched ? "verified" : "unverified");
  return result;
}

void __fastcall initialize_stage(void* stage) {
  const auto* owner = current_owner();
  const auto* pool = current_pool(owner);
  const std::uint32_t expected = pool ? field<std::uint16_t>(pool, 208) : 0;
  const bool recognized = accessible(stage, kStageSize) &&
                          field<const void*>(stage, 0) == stage_vtable && valid_cpu(pool, expected);
  // Observer only: native Init reads the CPU manager's actual count/128-byte stride. Do not
  // replace its allocation routine or request Init ourselves, including on a renderer reset.
  original_stage(stage);
  const bool readable = recognized && current_owner() == owner && current_pool(owner) == pool &&
                        accessible(stage, kStageSize) &&
                        field<const void*>(stage, 0) == stage_vtable;
  const auto count = readable ? field<std::uint32_t>(stage, kClothingBuffer + 96) : 0;
  const auto stride = readable ? field<std::uint32_t>(stage, kClothingBuffer + 100) : 0;
  const auto resource = readable ? field<std::uintptr_t>(stage, kClothingBuffer + 88) : 0;
  const bool matched = readable && matches_gpu_allocation(expected, count, stride, resource);
  if (matched) {
    ++verified_gpu_stages;
    last_gpu_count.store(count);
  }
  note(
      "MATERIAL capacity GPU stage=%p count=%u stride=%u resource=%s CPU=%u result=%s; visual "
      "acceptance unverified",
      stage, count, stride, resource ? "present" : "unverified", expected,
      matched ? "verified" : "unverified");
}

std::optional<unsigned> environment_scale() {
  char value[16]{};
  SetLastError(ERROR_SUCCESS);
  const DWORD length = GetEnvironmentVariableA(kScaleVariable, value, sizeof(value));
  if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
    return kDefaultScale;
  }
  if (length == 0 || length >= sizeof(value)) {
    return std::nullopt;
  }
  return parse_scale(std::string_view(value, length));
}

}  // namespace

bool install(std::uint8_t* image, std::size_t image_size, Logger logger) {
  std::lock_guard<std::mutex> lock(install_mutex);
  if (attempted) {
    return installed.load();
  }
  attempted = true;
  if (!logger || !image || image_size == 0) {
    return false;
  }
  note = logger;
  module_image = image;
  module_size = image_size;
  const auto scale = environment_scale();
  if (!scale) {
    note("MATERIAL capacity invalid %s; expected integer 1..8, hooks disabled", kScaleVariable);
    return false;
  }
  configured_scale = *scale;
  const auto* nt = headers();
  if (!nt) {
    note("MATERIAL capacity invalid module image; hooks disabled");
    return false;
  }
  auto* constructor = resolve_unique("clothing constructor", kPoolConstructor, nt);
  const auto* constructor_tail =
      resolve_unique("clothing constructor layout", kPoolConstructorTail, nt);
  auto* caller = resolve_unique("clothing factory call", kPoolCaller, nt);
  auto* startup = resolve_unique("material pools initialization", kPoolsInitialize, nt);
  auto* owner_constructor =
      resolve_unique("material manager constructor", kMaterialManagerConstructor, nt);
  auto* stage_init = resolve_unique("material stage initialization", kStageInitialize, nt);
  auto* stage_constructor = resolve_unique("material stage constructor", kStageConstructor, nt);
  if (!constructor || !constructor_tail || constructor_tail != constructor + 0xEC || !caller ||
      !startup || !owner_constructor || !stage_init || !stage_constructor ||
      !in_image(startup, 0x76) || !accessible(startup, 0x76) ||
      static_cast<std::size_t>(caller - module_image) < 0x60) {
    note("MATERIAL capacity native signatures missing or ambiguous; hooks disabled");
    return false;
  }
  pool_vtable = relative(constructor + 29, 3, 7);
  owner_vtable = relative(owner_constructor + 15, 3, 7);
  stage_vtable = relative(stage_constructor + 12, 3, 7);
  owner_slot = reinterpret_cast<const void* const*>(relative(startup + 0x2B, 3, 7));
  constructor_return = caller + 14;
  if (!in_image(pool_vtable, 48) || !accessible(pool_vtable, 48) || !in_image(owner_vtable, 264) ||
      !accessible(owner_vtable, 264) || !in_image(stage_vtable, 16) ||
      !accessible(stage_vtable, 16) || !in_image(owner_slot, sizeof(*owner_slot)) ||
      !accessible(owner_slot, sizeof(*owner_slot), true) ||
      reinterpret_cast<std::uintptr_t>(owner_slot) % alignof(void*) != 0 ||
      relative(caller + 9, 1, 5) != constructor || startup[0x71] != 0xE8 ||
      relative(startup + 0x71, 1, 5) != caller - 0x60) {
    note("MATERIAL capacity native ownership/layout validation failed; hooks disabled");
    return false;
  }
  // These methods are not called or hooked. Their exact implementations independently validate
  // the class's CPU vector offsets and entry size consumed by native GPU initialization.
  constexpr std::uint8_t count_bytes[] = {
      0x48, 0x8B, 0x81, 0x98, 0, 0, 0, 0x48, 0x2B, 0x81, 0x90, 0, 0, 0, 0x48, 0xC1, 0xF8, 7, 0xC3};
  constexpr std::uint8_t stride_bytes[] = {0xB8, 0x80, 0, 0, 0, 0xC3};
  const auto* count_method = field<const void*>(pool_vtable, 24);
  const auto* stride_method = field<const void*>(pool_vtable, 32);
  if (!in_image(count_method, sizeof(count_bytes)) ||
      !accessible(count_method, sizeof(count_bytes)) ||
      std::memcmp(count_method, count_bytes, sizeof(count_bytes)) != 0 ||
      !in_image(stride_method, sizeof(stride_bytes)) ||
      !accessible(stride_method, sizeof(stride_bytes)) ||
      std::memcmp(stride_method, stride_bytes, sizeof(stride_bytes)) != 0) {
    note("MATERIAL capacity native count/stride methods changed; hooks disabled");
    return false;
  }
  // Validate the class's virtual Init thunk, but hook its own implementation only.
  const auto* stage_entry = field<const std::uint8_t*>(stage_vtable, 8);
  if (!in_image(stage_entry, 5) || !accessible(stage_entry, 5) || stage_entry[0] != 0xE9 ||
      relative(stage_entry, 1, 5) != stage_init) {
    note("MATERIAL capacity native GPU class mismatch; hooks disabled");
    return false;
  }
  if (const auto* owner = current_owner(); owner && field<const void*>(owner, 192)) {
    note("MATERIAL capacity existing pool detected; it remains unchanged and requires restart");
  }
  const auto initialized = MH_Initialize();
  if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
    note("MATERIAL capacity MinHook initialization failed=%d", static_cast<int>(initialized));
    return false;
  }
  const auto created = MH_CreateHook(constructor, reinterpret_cast<void*>(construct_pool),
                                     reinterpret_cast<void**>(&original_construct));
  if (created != MH_OK) {
    note("MATERIAL capacity constructor hook creation failed=%d", static_cast<int>(created));
    return false;
  }
  const auto observer_created = MH_CreateHook(stage_init, reinterpret_cast<void*>(initialize_stage),
                                              reinterpret_cast<void**>(&original_stage));
  if (observer_created != MH_OK) {
    MH_RemoveHook(constructor);
    original_construct = nullptr;
    note("MATERIAL capacity GPU observer creation failed=%d", static_cast<int>(observer_created));
    return false;
  }
  const auto observer_enabled = MH_EnableHook(stage_init);
  const auto enabled = observer_enabled == MH_OK ? MH_EnableHook(constructor) : observer_enabled;
  if (enabled != MH_OK) {
    MH_RemoveHook(constructor);
    MH_RemoveHook(stage_init);
    original_construct = nullptr;
    original_stage = nullptr;
    note("MATERIAL capacity hook enable failed=%d; hooks removed", static_cast<int>(enabled));
    return false;
  }
  installed.store(true);
  note(
      "MATERIAL capacity hooks installed scale=%u; pending natural native startup; existing pools "
      "unchanged",
      configured_scale);
  return true;
}

void report_status() {
  std::lock_guard<std::mutex> lock(install_mutex);
  if (!note) {
    return;
  }
  note(
      "MATERIAL capacity status installed=%d scale=%u constructors=%u increased=%u skipped=%u "
      "GPU_verified=%u CPU_count=%u GPU_count=%u; live hair acceptance unverified",
      installed.load(), configured_scale, observed_constructions.load(),
      increased_constructions.load(), skipped_constructions.load(), verified_gpu_stages.load(),
      last_cpu_count.load(), last_gpu_count.load());
}

}  // namespace kcds::runtime::characters::materials
