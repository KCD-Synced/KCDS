#include "model_collision.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../branding/runtime_branding.hpp"
#include "cgf_proxies.hpp"

namespace kcds::runtime::props::model_collision {
namespace {

// ICryPak: FOpen(path, mode, flags), FReadRaw(data, 1, count, file), FGetSize(file),
// FClose(file), by their byte offsets in its vtable.
constexpr std::size_t kPakOpen = 288;
constexpr std::size_t kPakRead = 304;
constexpr std::size_t kPakSize = 368;
constexpr std::size_t kPakClose = 440;
// The largest model file read (the proxies are in the small .cgf, the meshes in its .cgfm).
constexpr std::size_t kMostFileBytes = 8 * 1024 * 1024;

// gEnv->pPhysicalWorld GetGeomManager (slot 4); the manager's LoadPhysGeometry (14) and
// UnregisterGeometry (9); IPhysicalEntity AddGeometry (8).
constexpr std::size_t kGetGeomManager = 4;
constexpr std::size_t kLoadPhysGeometry = 14;
constexpr std::size_t kUnregisterGeometry = 9;
constexpr std::size_t kAddGeometry = 8;

// CMemStream as the geometry loaders read it: the buffer at + 8, its size + 16, the place read
// at + 20.
constexpr std::size_t kStreamBytes = 64;
constexpr std::size_t kStreamBuffer = 8;
constexpr std::size_t kStreamSize = 16;
constexpr std::size_t kStreamPlace = 20;
// strided_pointer: a pointer and a stride; passed by address.
constexpr std::size_t kStridedBytes = 16;

// pe_geomparams (as AddGeometry reads it).
constexpr std::size_t kGeomParamsBytes = 128;
constexpr std::size_t kGeomPosition = 12;
constexpr std::size_t kGeomRotation = 24;
constexpr std::size_t kGeomScale = 40;
constexpr std::size_t kGeomSurface = 64;
constexpr std::size_t kGeomFlags = 68;
constexpr std::size_t kGeomColliderFlags = 72;
constexpr std::size_t kGeomMinContact = 76;
constexpr std::size_t kGeomBreakable = 80;
constexpr std::size_t kGeomRecalcBounds = 108;
constexpr std::uint32_t kUnusedInt = 0x80000000u;
constexpr std::uint32_t kUnusedFloat = 0xFFBFFFFFu;
// The flags the game's own proxies carry on a static body (a fence's parts read live).
constexpr std::uint32_t kProxyFlags = 0x0001FFFF;
constexpr std::uint32_t kProxyColliderFlags = 0x00000001;

void* const* world_global = nullptr;
Logger log_line = nullptr;

template <class Fn>
Fn slot_at(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

template <class Fn>
Fn slot_by_offset(void* object, std::size_t offset) {
  return slot_at<Fn>(object, offset / sizeof(void*));
}

template <class T>
void put(std::uint8_t* buffer, std::size_t at, const T& value) {
  std::memcpy(buffer + at, &value, sizeof(T));
}

bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
  void* pak = branding::game_pak();
  if (!pak) {
    return false;
  }
  using Open = void*(__fastcall*)(void* pak, const char* path, const char* mode, unsigned flags);
  using Read = std::size_t(__fastcall*)(void* pak, void* data, std::size_t size, std::size_t count,
                                        void* file);
  using Size = std::size_t(__fastcall*)(void* pak, void* file);
  using Close = int(__fastcall*)(void* pak, void* file);
  void* file = slot_by_offset<Open>(pak, kPakOpen)(pak, path.c_str(), "rb", 0);
  if (!file) {
    return false;
  }
  const std::size_t size = slot_by_offset<Size>(pak, kPakSize)(pak, file);
  bool done = false;
  if (size > 0 && size <= kMostFileBytes) {
    out.resize(size);
    done = slot_by_offset<Read>(pak, kPakRead)(pak, out.data(), 1, size, file) == size;
  }
  slot_by_offset<Close>(pak, kPakClose)(pak, file);
  return done;
}

// Adds one proxy (`data`, `size` bytes) at `where` to the body; false when the game made nothing.
bool add_proxy(void* manager, void* body, const std::uint8_t* data, std::uint32_t size,
               const cgf::Transform& where, float scale) {
  alignas(16) std::uint8_t stream[kStreamBytes] = {};
  put(stream, kStreamBuffer, data);
  put(stream, kStreamSize, std::int32_t(size));
  put(stream, kStreamPlace, std::int32_t(0));
  // No vertices, indices or materials of the caller: a proxy's data holds its own.
  alignas(16) std::uint8_t none[kStridedBytes] = {};
  using LoadPhysGeometry =
      void*(__fastcall*)(void* manager, void* stream, void* vertices, void* indices, void* ids);
  void* geometry =
      slot_at<LoadPhysGeometry>(manager, kLoadPhysGeometry)(manager, stream, none, none, nullptr);
  if (!geometry) {
    return false;
  }
  alignas(16) std::uint8_t params[kGeomParamsBytes] = {};
  const std::array<float, 3> place{where.place[0] * scale, where.place[1] * scale,
                                   where.place[2] * scale};
  put(params, kGeomPosition, place);
  put(params, kGeomRotation, cgf::quaternion_of(where.rows));
  put(params, kGeomScale, scale);
  put(params, kGeomSurface, kUnusedInt);
  put(params, kGeomFlags, kProxyFlags);
  put(params, kGeomColliderFlags, kProxyColliderFlags);
  put(params, kGeomMinContact, kUnusedFloat);
  put(params, kGeomBreakable, kUnusedInt);
  put(params, kGeomRecalcBounds, std::int32_t(1));
  using AddGeometry =
      int(__fastcall*)(void* body, void* geometry, void* params, int id, int thread_safe);
  const int part = slot_at<AddGeometry>(body, kAddGeometry)(body, geometry, params, -1, 0);
  // The body holds its own reference now.
  using Unregister = int(__fastcall*)(void* manager, void* geometry);
  slot_at<Unregister>(manager, kUnregisterGeometry)(manager, geometry);
  return part >= 0;
}

}  // namespace

void install(void* const* physical_world, Logger logger) {
  world_global = physical_world;
  log_line = logger;
}

int add(void* body, const std::string& model, float scale) {
  void* world = world_global ? *world_global : nullptr;
  if (!body || !world || model.empty()) {
    return 0;
  }
  std::vector<std::uint8_t> file;
  if (!read_file(model, file)) {
    return 0;
  }
  const std::vector<cgf::Proxy> proxies = cgf::proxies_of(file);
  if (proxies.empty()) {
    return 0;
  }
  using GetGeomManager = void*(__fastcall*)(void* world);
  void* manager = slot_at<GetGeomManager>(world, kGetGeomManager)(world);
  if (!manager) {
    return 0;
  }
  int added = 0;
  for (const cgf::Proxy& proxy : proxies) {
    if (add_proxy(manager, body, file.data() + proxy.offset, proxy.size, proxy.where, scale)) {
      ++added;
    }
  }
  if (log_line) {
    log_line("PROP collision from %s: %d of %zu proxies", model.c_str(), added, proxies.size());
  }
  return added;
}

}  // namespace kcds::runtime::props::model_collision
