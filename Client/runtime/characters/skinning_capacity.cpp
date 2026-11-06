#include "skinning_capacity.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>

#include "MinHook.h"
#include "kcd2s/bridge/sigscan.hpp"
#include "skinning_capacity_plan.hpp"

namespace kcds::runtime::characters::skinning {
namespace {

namespace bridge = kcd2s::bridge;

// WHGame 1.5.5, live: CComputeSkinningStage::Init. Each pattern matched
// exactly once in executable sections of SHA256
// bdf8f9e4a11257a72b64c84700e284c29e4c4ccaf5b8d4bfa7d0b2a7294479f7.
// No addresses below are used to resolve natives; relative operands and layouts are validated.
constexpr const char* kInitPattern =
    "48 89 5C 24 10 57 48 81 EC 80 04 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 70 04 00 "
    "00 8B 15 ?? ?? ?? ?? 48 8B D9 33 FF 48 83 C1 30 48 89 7C 24 28 45 33 C9 41 B8 20 00 00 00 "
    "C7 44 24 20 40 00 20 20 E8";
constexpr const char* kBonePattern =
    "8B 15 ?? ?? ?? ?? 48 8B D9 33 FF 48 83 C1 30 48 89 7C 24 28 45 33 C9 41 B8 20 00 00 00 "
    "C7 44 24 20 40 00 20 20 E8";
constexpr const char* kMorphPattern =
    "8B 15 ?? ?? ?? ?? 48 8D 8B 28 01 00 00 45 33 C9 48 89 7C 24 28 41 B8 08 00 00 00 "
    "C7 44 24 20 40 00 20 20 E8";
constexpr const char* kScaleVariable = "KCDS_SKINNING_CAPACITY_SCALE";
constexpr std::size_t kStageSize = 416;
constexpr std::size_t kBufferOffsets[] = {48, 168, 296};
constexpr std::size_t kResourceOffset = 88;
constexpr std::size_t kCountOffset = 96;
constexpr std::size_t kStrideOffset = 100;

using InitializeStage = void(__fastcall*)(void*);
InitializeStage original_init = nullptr;
std::int32_t* bone_capacity = nullptr;
std::int32_t* morph_capacity = nullptr;
std::uint8_t* module_image = nullptr;
std::size_t module_size = 0;
Logger note = nullptr;
unsigned configured_scale = kDefaultScale;
std::mutex install_mutex;
// Stage initialization normally runs on the render lifecycle. Serialize our shared cvar writes
// and the original's three reads, including a possible nested pipeline initialization.
std::recursive_mutex init_mutex;
bool attempted = false;
std::atomic<bool> installed{false};
std::atomic<std::uint32_t> initialized_stages{0};
std::atomic<std::uint32_t> increased_stages{0};
std::atomic<std::uint32_t> skipped_stages{0};
std::atomic<std::uint32_t> last_bones{0};
std::atomic<std::uint32_t> last_previous_bones{0};
std::atomic<std::uint32_t> last_morphs{0};

bool in_image(const void* pointer, std::size_t size) {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  const auto base = reinterpret_cast<std::uintptr_t>(module_image);
  return address >= base && address - base <= module_size && size <= module_size - (address - base);
}

bool accessible(const void* pointer, std::size_t size, bool writable) {
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

const IMAGE_NT_HEADERS64* headers() {
  if (!in_image(module_image, sizeof(IMAGE_DOS_HEADER)) ||
      !accessible(module_image, sizeof(IMAGE_DOS_HEADER), false)) {
    return nullptr;
  }
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
      static_cast<std::size_t>(dos->e_lfanew) > module_size) {
    return nullptr;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module_image + dos->e_lfanew);
  if (!in_image(nt, sizeof(*nt)) || !accessible(nt, sizeof(*nt), false) ||
      nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
      nt->OptionalHeader.SizeOfImage != module_size || nt->FileHeader.NumberOfSections == 0) {
    return nullptr;
  }
  const auto section_bytes = sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections;
  if (!in_image(IMAGE_FIRST_SECTION(nt), section_bytes) ||
      !accessible(IMAGE_FIRST_SECTION(nt), section_bytes, false)) {
    return nullptr;
  }
  return nt;
}

bool data_slot(const std::int32_t* slot, const IMAGE_NT_HEADERS64* nt) {
  if (!in_image(slot, sizeof(*slot)) ||
      reinterpret_cast<std::uintptr_t>(slot) % alignof(std::int32_t) != 0 ||
      !accessible(slot, sizeof(*slot), true)) {
    return false;
  }
  const auto offset = reinterpret_cast<const std::uint8_t*>(slot) - module_image;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto& candidate = section[i];
    if ((candidate.Characteristics & IMAGE_SCN_MEM_WRITE) == 0 ||
        (candidate.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
      continue;
    }
    const auto begin = std::size_t{candidate.VirtualAddress};
    const auto size = std::size_t{candidate.Misc.VirtualSize};
    if (begin > module_size || size > module_size - begin) {
      return false;
    }
    if (static_cast<std::size_t>(offset) >= begin &&
        static_cast<std::size_t>(offset) - begin <= size &&
        sizeof(*slot) <= size - (static_cast<std::size_t>(offset) - begin)) {
      return true;
    }
  }
  return false;
}

void* resolve_unique(const char* name, const char* pattern, const IMAGE_NT_HEADERS64* nt) {
  const auto signature = bridge::Signature::parse(pattern);
  if (!signature) {
    return nullptr;
  }
  std::uint8_t* match = nullptr;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto& candidate = section[i];
    if ((candidate.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
      continue;
    }
    const auto size = std::size_t{candidate.Misc.VirtualSize};
    if (candidate.VirtualAddress > module_size || size > module_size - candidate.VirtualAddress) {
      return nullptr;
    }
    const auto* begin = module_image + candidate.VirtualAddress;
    if (!accessible(begin, size, false)) {
      return nullptr;
    }
    std::size_t cursor = 0;
    while (cursor < size) {
      const auto offset = bridge::scan(begin + cursor, size - cursor, *signature);
      if (offset == bridge::knpos) {
        break;
      }
      if (match) {
        note("SKINNING capacity ambiguous signature=%s; native budgets unchanged", name);
        return nullptr;
      }
      match = module_image + candidate.VirtualAddress + cursor + offset;
      cursor += offset + 1;
    }
  }
  return match;
}

std::int32_t* operand_slot(const std::uint8_t* instruction) {
  if (!in_image(instruction, 6)) {
    return nullptr;
  }
  const auto offset = static_cast<std::size_t>(instruction - module_image);
  const auto target = bridge::rip_target(module_image, module_size, offset + 2, offset + 6);
  if (target == bridge::knpos || target > module_size ||
      sizeof(std::int32_t) > module_size - target) {
    return nullptr;
  }
  return reinterpret_cast<std::int32_t*>(module_image + target);
}

template <typename T>
T field(const std::uint8_t* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, object + offset, sizeof(value));
  return value;
}

bool pristine_stage(const std::uint8_t* stage) {
  for (const auto buffer : kBufferOffsets) {
    if (field<std::uintptr_t>(stage, buffer + kResourceOffset) != 0 ||
        field<std::uint32_t>(stage, buffer + kCountOffset) != 0) {
      return false;
    }
  }
  return true;
}

void __fastcall initialize_stage(void* pointer) {
  std::lock_guard<std::recursive_mutex> lock(init_mutex);
  auto* stage = static_cast<std::uint8_t*>(pointer);
  bool planned = false;
  CapacityPlan plan;
  if (!accessible(stage, kStageSize, false) || !pristine_stage(stage)) {
    ++skipped_stages;
    note(
        "SKINNING capacity stage=%p already allocated or unreadable; no override; restart needed "
        "for existing stages",
        pointer);
  } else if (!accessible(bone_capacity, sizeof(*bone_capacity), true) ||
             !accessible(morph_capacity, sizeof(*morph_capacity), true)) {
    ++skipped_stages;
    note("SKINNING capacity storage unavailable; forwarding native initialization unchanged");
  } else {
    const auto before_bones = *bone_capacity;
    const auto before_morphs = *morph_capacity;
    plan = make_plan(configured_scale, before_bones, before_morphs);
    planned = plan.valid();
    if (!planned) {
      ++skipped_stages;
      note("SKINNING capacity rejected native counts bones=%d morphs=%d; no override", before_bones,
           before_morphs);
    } else if (plan.result == PlanResult::increase) {
      *bone_capacity = static_cast<std::int32_t>(plan.effective.bones);
      *morph_capacity = static_cast<std::int32_t>(plan.effective.morphs);
      note("SKINNING capacity pre-init stage=%p scale=%u bones=%d->%u morphs=%d->%u bytes=%llu",
           pointer, configured_scale, before_bones, plan.effective.bones, before_morphs,
           plan.effective.morphs,
           static_cast<unsigned long long>(allocation_bytes(plan.effective)));
    }
  }

  // Native lifecycle only: never reinitialize or hot-resize an existing stage ourselves.
  // Its resource-allocation failure behavior is unchanged; catching an exception cannot safely
  // roll back a partially initialized native stage.
  original_init(pointer);
  ++initialized_stages;
  if (!accessible(stage, kStageSize, false)) {
    note("SKINNING capacity stage=%p post-init unreadable; allocation unverified", pointer);
    return;
  }
  const auto bones = field<std::uint32_t>(stage, kBufferOffsets[0] + kCountOffset);
  const auto previous_bones = field<std::uint32_t>(stage, kBufferOffsets[1] + kCountOffset);
  const auto morphs = field<std::uint32_t>(stage, kBufferOffsets[2] + kCountOffset);
  last_bones.store(bones);
  last_previous_bones.store(previous_bones);
  last_morphs.store(morphs);
  const bool valid = field<std::uint32_t>(stage, kBufferOffsets[0] + kStrideOffset) == 32 &&
                     field<std::uint32_t>(stage, kBufferOffsets[1] + kStrideOffset) == 32 &&
                     field<std::uint32_t>(stage, kBufferOffsets[2] + kStrideOffset) == 8 &&
                     field<std::uintptr_t>(stage, kBufferOffsets[0] + kResourceOffset) != 0 &&
                     field<std::uintptr_t>(stage, kBufferOffsets[1] + kResourceOffset) != 0 &&
                     field<std::uintptr_t>(stage, kBufferOffsets[2] + kResourceOffset) != 0;
  const bool matches = planned && valid && bones == plan.effective.bones &&
                       previous_bones == plan.effective.bones && morphs == plan.effective.morphs;
  if (matches && plan.result == PlanResult::increase) {
    ++increased_stages;
  }
  note(
      "SKINNING capacity allocated stage=%p bones=%u previous_bones=%u morphs=%u resources=%s "
      "plan=%s; rendering load acceptance remains unverified",
      pointer, bones, previous_bones, morphs, valid ? "present" : "invalid",
      matches ? "matched" : "unverified");
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
    note("SKINNING capacity invalid %s; expected integer 1..8, hook disabled", kScaleVariable);
    return false;
  }
  configured_scale = *scale;
  const auto* nt = headers();
  if (!nt) {
    note("SKINNING capacity invalid module image; hook disabled");
    return false;
  }
  auto* target = static_cast<std::uint8_t*>(resolve_unique("stage init", kInitPattern, nt));
  const auto* bones =
      static_cast<const std::uint8_t*>(resolve_unique("bone capacity", kBonePattern, nt));
  const auto* morphs =
      static_cast<const std::uint8_t*>(resolve_unique("morph capacity", kMorphPattern, nt));
  if (!target || !bones || !morphs || bones != target + 0x1F || morphs != target + 0x13E) {
    note("SKINNING capacity native signatures missing or layout changed; hook disabled");
    return false;
  }
  bone_capacity = operand_slot(bones);
  morph_capacity = operand_slot(morphs);
  if (!data_slot(bone_capacity, nt) || !data_slot(morph_capacity, nt) ||
      reinterpret_cast<std::uintptr_t>(morph_capacity) + sizeof(std::int32_t) !=
          reinterpret_cast<std::uintptr_t>(bone_capacity)) {
    note("SKINNING capacity RIP storage validation failed; hook disabled");
    return false;
  }
  // Before renderer cvar registration both slots are zero. Do not write defaults here: the
  // engine still needs to apply its config. Init validates the final values before any override.
  if ((*bone_capacity != 0 || *morph_capacity != 0) &&
      !make_plan(configured_scale, *bone_capacity, *morph_capacity).valid()) {
    note("SKINNING capacity implausible startup values bones=%d morphs=%d; hook disabled",
         *bone_capacity, *morph_capacity);
    return false;
  }
  const auto initialized = MH_Initialize();
  if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
    note("SKINNING capacity MinHook initialization failed=%d", static_cast<int>(initialized));
    return false;
  }
  const auto created = MH_CreateHook(target, reinterpret_cast<void*>(initialize_stage),
                                     reinterpret_cast<void**>(&original_init));
  if (created != MH_OK) {
    note("SKINNING capacity hook creation failed=%d", static_cast<int>(created));
    return false;
  }
  const auto enabled = MH_EnableHook(target);
  if (enabled != MH_OK) {
    MH_RemoveHook(target);
    original_init = nullptr;
    note("SKINNING capacity hook enable failed=%d", static_cast<int>(enabled));
    return false;
  }
  installed.store(true);
  note(
      "SKINNING capacity hook installed scale=%u; allocations pending native stage init; "
      "existing stages unchanged and require restart",
      configured_scale);
  return true;
}

Status status() {
  std::lock_guard<std::mutex> lock(install_mutex);
  return {installed.load(),           configured_scale,      initialized_stages.load(),
          increased_stages.load(),    skipped_stages.load(), last_bones.load(),
          last_previous_bones.load(), last_morphs.load()};
}

void report_status() {
  const auto current = status();
  if (!note) {
    return;
  }
  note(
      "SKINNING capacity status installed=%d scale=%u initialized=%u increased=%u skipped=%u "
      "bones=%u previous_bones=%u morphs=%u state=%s",
      current.installed, current.scale, current.initialized_stages, current.increased_stages,
      current.skipped_stages, current.last_bones, current.last_previous_bones, current.last_morphs,
      current.initialized_stages ? "observed; load acceptance unverified"
                                 : "not observed; existing stages need restart");
}

}  // namespace kcds::runtime::characters::skinning
