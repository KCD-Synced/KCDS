#include "../core/client_internal.hpp"

#include "prefab_probe.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "../core/log.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::runtime::testing::prefab_probe {
namespace {

using kcds::game_client::log;

// WHGame.dll 1.5.5 at its. A test-only probe names the functions by their
// address; the mod's own code finds them by signature once the path is proven.
constexpr std::uintptr_t kIdaBase = 0x180000000ULL;
// CryGUID from its text, "{...}" optional (true on success).
constexpr std::uintptr_t kParseGuid = 0x180719B1CULL;
// The prefab template registry (its map of templates by GUID is the object itself).
constexpr std::uintptr_t kTemplateRegistry = 0x181787564ULL;
// The template of a GUID in the registry, null when none.
constexpr std::uintptr_t kFindTemplate = 0x1807EAB9CULL;
// The game's prefab spawner (the global's + 336): spawn(manager, guid, entity id, max spawns,
// spawn its contents), as Game.SpawnPrefab calls it.
constexpr std::uintptr_t kGameGlobal = 0x18549D300ULL;
constexpr std::size_t kPrefabManager = 336;
constexpr std::uintptr_t kSpawnPrefab = 0x1809E4D9CULL;
// The manager's map of runtime prefabs by entity id (at + 8) and its find; a found node keeps the
// runtime prefab at + 40, whose entity ids are a vector at + 64 and render nodes at + 88.
constexpr std::size_t kRuntimeMap = 8;
constexpr std::uintptr_t kFindRuntime = 0x180689888ULL;
constexpr std::size_t kNodeValue = 40;
constexpr std::size_t kEntityIds = 64;
constexpr std::size_t kRenderNodes = 88;
// The template built for spawning (the manager's cache, built on first use): its brushes are a
// vector of 112-byte entries at + 24, each with its RndFlags at + 16 (reads them).
constexpr std::uintptr_t kBuiltTemplate = 0x1809E5788ULL;
constexpr std::size_t kTemplateBrushes = 24;
constexpr std::size_t kBrushSize = 112;
constexpr std::size_t kBrushFlags = 16;
// What the game's reader makes of an RndFlags value wider than 32 bits.
constexpr std::uint32_t kSaturatedFlags = 0xFFFFFFFFu;
// A render node's flags, a brush's physical entity and its Physicalize(instant) slot.
constexpr std::size_t kNodeFlags = 40;
constexpr std::size_t kBrushPhysics = 128;
constexpr std::size_t kPhysicalizeSlot = 33;
// IEntity::GetId.
constexpr std::size_t kEntityGetId = 1;

struct Guid {
  std::uint64_t low = 0;
  std::uint64_t high = 0;
};

using ParseGuid = bool(__fastcall*)(const char* text, Guid* guid);
using TemplateRegistry = void*(__fastcall*)();
using FindTemplate = void*(__fastcall*)(void* registry, const Guid* guid);
using SpawnPrefab = std::int64_t(__fastcall*)(void* manager, const Guid* guid, std::uint32_t id,
                                              int max_spawn, bool contents);
using BuiltTemplate = std::uint8_t*(__fastcall*)(void* manager, const Guid* guid);
using FindRuntime = std::uint8_t*(__fastcall*)(void* map, const std::uint32_t* id);
using Physicalize = void(__fastcall*)(void* node, bool instant);
using GetId = std::uint32_t(__fastcall*)(void* entity);

HANDLE event = nullptr;
int probes = 0;

template <typename Pointer>
Pointer at(std::uintptr_t ida_address) {
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"WHGame.dll"));
  return reinterpret_cast<Pointer>(base + (ida_address - kIdaBase));
}

std::size_t vector_count(const std::uint8_t* object, std::size_t offset, std::size_t size) {
  const auto begin = *reinterpret_cast<const std::uintptr_t*>(object + offset);
  const auto end = *reinterpret_cast<const std::uintptr_t*>(object + offset + 8);
  if (!begin || end < begin) {
    return 0;
  }
  return (end - begin) / size;
}

// Puts `flags` in place of a brush's saturated RndFlags in the built template; the count fixed.
int fix_saturated_flags(void* manager, const Guid& guid, std::uint32_t flags) {
  std::uint8_t* built = at<BuiltTemplate>(kBuiltTemplate)(manager, &guid);
  if (!built) {
    return -1;
  }
  auto* begin = *reinterpret_cast<std::uint8_t**>(built + kTemplateBrushes);
  auto* end = *reinterpret_cast<std::uint8_t**>(built + kTemplateBrushes + 8);
  int fixed = 0;
  for (std::uint8_t* brush = begin; brush && brush < end; brush += kBrushSize) {
    auto* value = reinterpret_cast<std::uint32_t*>(brush + kBrushFlags);
    if (*value == kSaturatedFlags) {
      *value = flags;
      ++fixed;
    }
  }
  return fixed;
}

void run(const std::string& guid_text, const float position[3], std::uint32_t fix_flags) {
  Guid guid;
  if (!at<ParseGuid>(kParseGuid)(guid_text.c_str(), &guid)) {
    log("PREFABPROBE guid %s unreadable", guid_text.c_str());
    return;
  }
  void* registry = at<TemplateRegistry>(kTemplateRegistry)();
  void* found = registry ? at<FindTemplate>(kFindTemplate)(registry, &guid) : nullptr;
  log("PREFABPROBE guid %s = %016llx %016llx registry=%p template=%p", guid_text.c_str(),
      static_cast<unsigned long long>(guid.low), static_cast<unsigned long long>(guid.high),
      registry, found);
  std::uint8_t* global = *at<std::uint8_t**>(kGameGlobal);
  void* manager = global ? *reinterpret_cast<void**>(global + kPrefabManager) : nullptr;
  if (!manager) {
    log("PREFABPROBE no prefab manager");
    return;
  }
  if (fix_flags != 0) {
    log("PREFABPROBE saturated RndFlags set to %08x: %d brushes", fix_flags,
        fix_saturated_flags(manager, guid, fix_flags));
  }
  const float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const std::string name = "kcds_prefab_probe_" + std::to_string(++probes);
  void* entity = props::entity::spawn_class_with(name, "RuntimePrefab", position, rotation, {});
  if (!entity) {
    log("PREFABPROBE RuntimePrefab entity not spawned");
    return;
  }
  const auto get_id = reinterpret_cast<GetId>((*static_cast<void***>(entity))[kEntityGetId]);
  std::uint32_t id = get_id(entity);
  const std::int64_t result = at<SpawnPrefab>(kSpawnPrefab)(manager, &guid, id, 0, true);
  auto* map = static_cast<std::uint8_t*>(manager) + kRuntimeMap;
  std::uint8_t* node = at<FindRuntime>(kFindRuntime)(map, &id);
  const std::uint8_t* head = *reinterpret_cast<std::uint8_t**>(map);
  const std::uint8_t* runtime = nullptr;
  if (node && node != head) {
    runtime = *reinterpret_cast<std::uint8_t**>(node + kNodeValue);
  }
  const std::size_t entities = runtime ? vector_count(runtime, kEntityIds, 4) : 0;
  const std::size_t renders = runtime ? vector_count(runtime, kRenderNodes, 8) : 0;
  log("PREFABPROBE spawned entity=%u at %.1f %.1f %.1f result=%lld runtime=%p entities=%zu "
      "render nodes=%zu",
      id, position[0], position[1], position[2], static_cast<long long>(result),
      static_cast<const void*>(runtime), entities, renders);
  if (!runtime) {
    return;
  }
  // Each render node's flags (IRenderNode + 40, set by the spawn from the brush's RndFlags) and
  // physical entity (CBrush + 128). A brush without one is physicalized now: CBrush slot 33,
  // Physicalize(instant) (->), which otherwise waits for on-demand physics.
  const auto* nodes = *reinterpret_cast<std::uint8_t* const* const*>(runtime + kRenderNodes);
  for (std::size_t index = 0; index < renders; ++index) {
    std::uint8_t* node = nodes[index];
    if (!node) {
      continue;
    }
    const std::uint64_t flags = *reinterpret_cast<const std::uint64_t*>(node + kNodeFlags);
    void* before = *reinterpret_cast<void**>(node + kBrushPhysics);
    if (!before) {
      const auto physicalize =
          reinterpret_cast<Physicalize>((*reinterpret_cast<void***>(node))[kPhysicalizeSlot]);
      physicalize(node, true);
    }
    void* after = *reinterpret_cast<void**>(node + kBrushPhysics);
    log("PREFABPROBE render node %zu flags=%016llx physics %p -> %p", index,
        static_cast<unsigned long long>(flags), before, after);
  }
}

}  // namespace

void install() {
  wchar_t name[96];
  swprintf_s(name, L"Local\\KCD2S.ProbePrefab.%lu", GetCurrentProcessId());
  event = CreateEventW(nullptr, FALSE, FALSE, name);
}

void tick(bool in_world) {
  if (!event || !in_world || WaitForSingleObject(event, 0) != WAIT_OBJECT_0) {
    return;
  }
  wchar_t folder[MAX_PATH];
  GetTempPathW(MAX_PATH, folder);
  wchar_t file[MAX_PATH];
  swprintf_s(file, L"%skcds-prefab-%lu.txt", folder, GetCurrentProcessId());
  std::ifstream input(file);
  std::string guid;
  float position[3] = {};
  if (!(input >> guid >> position[0] >> position[1] >> position[2])) {
    log("PREFABPROBE request unreadable");
    return;
  }
  // An optional fifth value: the RndFlags (hex) for brushes whose flags the game read as all set.
  std::string fix;
  input >> fix;
  const auto fix_flags = static_cast<std::uint32_t>(std::strtoul(fix.c_str(), nullptr, 16));
  run(guid, position, fix_flags);
}

}  // namespace kcds::runtime::testing::prefab_probe
