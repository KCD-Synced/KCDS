#include "aim.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace kcds::runtime::ranged {
namespace {

// ISkeletonPose: the aim pose blender (CPoseBlenderAim, at pose+0xE0; slots 41 and 42 return it,
// the look blender's are 43 and 44).
constexpr std::size_t kPoseAimSlot = 41;
// CPoseBlenderAim (IAnimationPoseBlenderDir): SetState writes the state byte, SetTarget the
// target.
constexpr std::size_t kSetStateSlot = 8;
constexpr std::size_t kSetTargetSlot = 9;
constexpr std::size_t kState = 8;
constexpr std::size_t kTarget = 12;

using SetState = void(__fastcall*)(void* aim, char enabled);
using SetTarget = std::intptr_t(__fastcall*)(void* aim, const kcd2s::core::Vec3* target);

AimNatives natives;
AimLogger write_log = nullptr;
// The blenders' table, once its two entries are this module's.
void** table = nullptr;
SetState original_set_state = nullptr;
SetTarget original_set_target = nullptr;

std::mutex replicas_mutex;
std::unordered_map<void*, AimState> replicas;

bool finite(const kcd2s::core::Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool replica_aim(void* aim, AimState& state) {
  std::lock_guard<std::mutex> lock(replicas_mutex);
  auto it = replicas.find(aim);
  if (it == replicas.end()) {
    return false;
  }
  state = it->second;
  return true;
}

void __fastcall set_state(void* aim, char enabled) {
  AimState owner;
  if (replica_aim(aim, owner)) {
    enabled = owner.enabled ? 1 : 0;
  }
  original_set_state(aim, enabled);
}

std::intptr_t __fastcall set_target(void* aim, const kcd2s::core::Vec3* target) {
  AimState owner;
  if (replica_aim(aim, owner)) {
    target = &owner.target;
  }
  return original_set_target(aim, target);
}

// Whether `size` bytes at `address` can be read: the skeleton pose's aim slot holds no blender
// in a character's first frames, and what it holds then is no address (the engine adapter
// stopped reading it).
bool readable(const void* address, std::size_t size) {
  if (reinterpret_cast<std::uintptr_t>(address) < 0x10000) {
    return false;
  }
  MEMORY_BASIC_INFORMATION region{};
  if (!VirtualQuery(address, &region, sizeof(region)) || region.State != MEM_COMMIT ||
      (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return false;
  }
  const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
  return reinterpret_cast<std::uintptr_t>(address) + size <= end;
}

// The blenders' table takes this module's two entries (every CPoseBlenderAim shares it; the
// entries pass through for a blender that is no replica's).
bool take_table(void* aim) {
  // The common case, every replica every frame: the game's own pointer, already known to be one.
  if (table) {
    return *static_cast<void***>(aim) == table;
  }
  if (!readable(aim, kTarget + sizeof(kcd2s::core::Vec3))) {
    return false;
  }
  void** found = *static_cast<void***>(aim);
  if (!readable(found, (kSetTargetSlot + 1) * sizeof(void*)) ||
      found[kSetTargetSlot] != natives.set_target) {
    return false;
  }
  DWORD old = 0;
  if (!VirtualProtect(&found[kSetStateSlot], 2 * sizeof(void*), PAGE_READWRITE, &old)) {
    return false;
  }
  original_set_state = reinterpret_cast<SetState>(found[kSetStateSlot]);
  original_set_target = reinterpret_cast<SetTarget>(found[kSetTargetSlot]);
  found[kSetStateSlot] = reinterpret_cast<void*>(set_state);
  found[kSetTargetSlot] = reinterpret_cast<void*>(set_target);
  VirtualProtect(&found[kSetStateSlot], 2 * sizeof(void*), old, &old);
  table = found;
  write_log("AIM pose blender table %p taken", static_cast<void*>(table));
  return true;
}

}  // namespace

bool install_aim(const AimNatives& given, AimLogger logger) {
  natives = given;
  write_log = logger;
  return natives.set_target != nullptr;
}

void* aim_of(void* skeleton_pose) {
  if (!skeleton_pose || !natives.set_target) {
    return nullptr;
  }
  using GetAim = void*(__fastcall*)(void*);
  void* aim = (*static_cast<GetAim**>(skeleton_pose))[kPoseAimSlot](skeleton_pose);
  return aim && take_table(aim) ? aim : nullptr;
}

AimState read_aim(void* aim) {
  AimState state;
  if (!aim) {
    return state;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(aim);
  state.enabled = bytes[kState] != 0;
  state.target = *reinterpret_cast<const kcd2s::core::Vec3*>(bytes + kTarget);
  if (!finite(state.target)) {
    state = AimState{};
  }
  return state;
}

void keep_replica_aim(void* aim, const AimState& owner) {
  if (!aim || !table || !finite(owner.target)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(replicas_mutex);
    replicas[aim] = owner;
  }
  original_set_target(aim, &owner.target);
  original_set_state(aim, owner.enabled ? 1 : 0);
}

void forget_replica_aim(void* aim) {
  std::lock_guard<std::mutex> lock(replicas_mutex);
  replicas.erase(aim);
}

}  // namespace kcds::runtime::ranged
