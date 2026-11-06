#include "game_prefabs.hpp"

#include <Windows.h>

#include <set>
#include <string>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "../core/natives.hpp"
#include "../props/prop_entity.hpp"

// After windows.h.
#include "MinHook.h"

namespace kcds::runtime::prefabs {
namespace {

namespace entity = props::entity;
using kcds::game_client::resolve_unique;

// The script binding Game.SpawnPrefab: its calls give the GUID reader (+ 39), the
// game global holding the prefab manager (+ 68) and the spawner (+ 109).
constexpr char kSpawnBinding[] =
    "48 89 5C 24 08 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 48 8B FA 49 8B "
    "C9 48 8D 54 24 30 49 8B D8 E8 ?? ?? ?? ?? 84 C0 74 42 48 8B 0D ?? ?? ?? ?? 8B D3 48 8B 01 "
    "FF 50 70 48 85 C0 74 2E 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 30 8A 84 24 88 00 00 00 44 8B C3 "
    "44 8B 8C 24 80 00 00 00 88 44 24 20 48 8B 89 50 01 00 00 E8";
constexpr int kGuidReaderCall = 40;
constexpr int kGuidReaderEnd = 44;
constexpr int kGlobalLoad = 71;
constexpr int kGlobalLoadEnd = 75;
constexpr int kSpawnerCall = 110;
constexpr int kSpawnerEnd = 114;
// The script binding Game.DeletePrefab: its call to the manager's delete (+ 53), the
// function right after it (Game.MovePrefab, starts with the same bytes and calls its
// own).
constexpr char kDeleteBinding[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 0D ?? ?? ?? ?? 48 8B FA 41 8B D0 49 8B D8 48 8B 01 FF "
    "50 70 48 85 C0 74 15 48 8B 0D ?? ?? ?? ?? 8B D3 48 8B 89 50 01 00 00 E8 16 00 00 00";
constexpr int kDeleteCall = 54;
constexpr int kDeleteEnd = 58;
// The spawner's first call (+ 29) builds the template (or takes the one built before).
constexpr std::size_t kBuilderCall = 29;
constexpr std::uint8_t kCallOpcode = 0xE8;
constexpr std::size_t kPrefabManager = 336;

// The built template's brushes: 112-byte entries at + 24 to + 32, each with its RndFlags at + 16.
// The built template's entities: 200-byte entries at + 0 to + 8, each starting with the
// SEntitySpawnParams the spawn copies, whose EntityGuid (+ 8) the template leaves 0.
constexpr std::size_t kTemplateEntities = 0;
constexpr std::size_t kTemplateEntitiesEnd = 8;
constexpr std::size_t kEntityEntrySize = 200;
constexpr std::size_t kEntityEntryGuid = 8;
constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ULL;
constexpr std::size_t kTemplateBrushes = 24;
constexpr std::size_t kTemplateBrushesEnd = 32;
constexpr std::size_t kBrushEntrySize = 112;
constexpr std::size_t kBrushEntryFlags = 16;
// What the game makes of a RndFlags value wider than 32 bits, and what such a brush gets instead
// (ERF_CASTSHADOWMAPS: the house casts its shadow as in the level).
constexpr std::uint32_t kSaturatedFlags = 0xFFFFFFFFu;
constexpr std::uint32_t kBrushFlags = 0x8;

// The manager's runtime prefabs by entity id: a map at + 8, found (the spawner's
// call at + 66), which returns the map's head when there is none; a node keeps the runtime prefab
// at + 40, whose render nodes are a vector at + 88 to + 96.
constexpr std::size_t kFindRuntimeCall = 66;
constexpr std::size_t kRuntimeMap = 8;
constexpr std::size_t kMapNodeValue = 40;
constexpr std::size_t kRuntimeNodes = 88;
constexpr std::size_t kRuntimeNodesEnd = 96;
// A prefab's brushes are COwnedBrush (a CBrush its prefab owns): both have Physicalize(instant) in
// slot 33 and their physical entity at + 128.
constexpr char kBrushClass[] = ".?AVCBrush@@";
constexpr char kOwnedBrushClass[] = ".?AVCOwnedBrush@@";
constexpr std::size_t kBrushPhysicalize = 33;
constexpr std::size_t kBrushPhysics = 128;
// The builder's calls to the template registry (+ 138) and to its find by GUID (+ 149);
// a registry template keeps the XML node of its objects at + 24.
constexpr std::size_t kRegistryCall = 138;
constexpr std::size_t kFindTemplateCall = 149;
constexpr std::size_t kTemplateObjects = 24;
// IXmlNode: getAttr(key) (slot 33, the value text, "" when it has none), getChildCount (44),
// getChild(XmlNodeRef* out, index) (45, which adds a reference), Release (4). A template's
// objects are read-only nodes (CXMLReadOnlyNode, the pak's binary XML), which cannot be changed.
constexpr std::size_t kXmlRelease = 4;
constexpr std::size_t kXmlGetAttr = 33;
constexpr std::size_t kXmlChildCount = 44;
constexpr std::size_t kXmlGetChild = 45;
// A nested prefab is built at most this deep (a door in a house in a farmstead).
constexpr int kMaxNesting = 4;
// IEntity::GetId.
constexpr std::size_t kEntityGetId = 1;
constexpr char kPrefabClass[] = "RuntimePrefab";
constexpr char kModelPrefix[] = "prefab:";

struct Guid {
  std::uint64_t low = 0;
  std::uint64_t high = 0;
};

using ReadGuid = bool(__fastcall*)(const char* text, Guid* guid);
using Spawner = std::int64_t(__fastcall*)(void* manager, const Guid* guid, std::uint32_t id,
                                          int max_spawns, bool contents);
using Builder = std::uint8_t*(__fastcall*)(void* manager, const Guid* guid);
using Delete = void(__fastcall*)(void* manager, std::uint32_t id);
using FindRuntime = std::uint8_t*(__fastcall*)(void* map, const std::uint32_t* id);
using Physicalize = void(__fastcall*)(void* brush, bool instant);
using GetId = std::uint32_t(__fastcall*)(void* entity);
using Registry = void*(__fastcall*)();
using FindTemplate = std::uint8_t*(__fastcall*)(void* registry, const Guid* guid);
using XmlGetAttr = const char*(__fastcall*)(void* node, const char* key);
using XmlChildCount = int(__fastcall*)(void* node);
using XmlGetChild = void**(__fastcall*)(void* node, void** out, int index);
using XmlRelease = void(__fastcall*)(void* node);

// A prefab a template holds (its "Prefab" object): its own template and its place and turn
// (x, y, z, w) in the holder's frame.
struct Nested {
  std::string guid;
  float position[3] = {};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

Logger log_line = nullptr;
ReadGuid read_guid = nullptr;
Spawner spawner = nullptr;
Builder builder = nullptr;
Delete delete_prefab = nullptr;
FindRuntime find_runtime = nullptr;
Registry registry = nullptr;
FindTemplate find_template = nullptr;
std::uint8_t** game_global = nullptr;
// The prefabs each template held, taken out of it before it was first built (the game's builder
// refuses a template that holds another prefab); by template GUID.
std::map<std::string, std::vector<Nested>> nested_of;
// The read-only XML node's getAttr, hooked while a template with nested prefabs is first built:
// a nested prefab's Type reads "" then, so the builder passes over it (as over any unknown type)
// instead of refusing the whole template.
XmlGetAttr original_get_attr = nullptr;
void* hooked_get_attr_target = nullptr;
bool hiding_nested = false;
// The templates that hold decals (passed over while built, get_attr_hiding_nested).
std::set<std::string> decal_templates;
// The names of the prefabs built inside each placed one, so they go with it.
std::map<std::string, std::vector<std::string>> inner_names;

// The target of the call instruction at `at`, null when there is no call there.
void* call_target(std::uint8_t* at) {
  if (!at || at[0] != kCallOpcode) {
    return nullptr;
  }
  std::int32_t offset = 0;
  std::memcpy(&offset, at + 1, sizeof(offset));
  return at + 5 + offset;
}

void* manager() {
  std::uint8_t* global = game_global ? *game_global : nullptr;
  return global ? *reinterpret_cast<void**>(global + kPrefabManager) : nullptr;
}

const char* rtti_name(void* object) {
  void** vtable = *static_cast<void***>(object);
  const auto* locator = static_cast<const std::uint8_t*>(vtable[-1]);
  if (!locator) {
    return "";
  }
  std::uint32_t descriptor = 0;
  std::memcpy(&descriptor, locator + 12, sizeof(descriptor));
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"WHGame.dll"));
  return reinterpret_cast<const char*>(base + descriptor + 16);
}

// Gives the built template's brushes read as ffffffff proper flags (once: the template stays
// built); the number fixed.
// The EntityGuid of the template's entity `index` in the prefab placed as `name`: the same in
// every player's game (the name is the server's prop and the nested prefab's place in it), so a
// door it holds is the same door everywhere (doors/doors.hpp keys doors by EntityGuid). Never 0.
std::uint64_t entity_guid(const std::string& name, std::size_t index) {
  std::uint64_t hash = kFnvOffset;
  const std::string key = name + "#" + std::to_string(index);
  for (const char c : key) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= kFnvPrime;
  }
  return hash == 0 ? 1 : hash;
}

// Gives the built template's entities the EntityGuids of the prefab placed as `name`, right
// before it is laid.
void name_entities(std::uint8_t* built, const std::string& name) {
  auto* begin = *reinterpret_cast<std::uint8_t**>(built + kTemplateEntities);
  auto* end = *reinterpret_cast<std::uint8_t**>(built + kTemplateEntitiesEnd);
  std::size_t index = 0;
  for (std::uint8_t* entry = begin; entry && entry < end; entry += kEntityEntrySize) {
    *reinterpret_cast<std::uint64_t*>(entry + kEntityEntryGuid) = entity_guid(name, index);
    ++index;
  }
}

int fix_saturated_flags(std::uint8_t* built) {
  auto* begin = *reinterpret_cast<std::uint8_t**>(built + kTemplateBrushes);
  auto* end = *reinterpret_cast<std::uint8_t**>(built + kTemplateBrushesEnd);
  int fixed = 0;
  for (std::uint8_t* brush = begin; brush && brush < end; brush += kBrushEntrySize) {
    auto* flags = reinterpret_cast<std::uint32_t*>(brush + kBrushEntryFlags);
    if (*flags == kSaturatedFlags) {
      *flags = kBrushFlags;
      ++fixed;
    }
  }
  return fixed;
}

// The runtime prefab the manager keeps for the entity, null when none.
std::uint8_t* runtime_of(void* prefab_manager, std::uint32_t id) {
  if (!find_runtime) {
    return nullptr;
  }
  auto* map = static_cast<std::uint8_t*>(prefab_manager) + kRuntimeMap;
  std::uint8_t* node = find_runtime(map, &id);
  const auto* head = *reinterpret_cast<std::uint8_t**>(map);
  if (!node || node == head) {
    return nullptr;
  }
  return *reinterpret_cast<std::uint8_t**>(node + kMapNodeValue);
}

// Physicalizes the runtime prefab's brushes at once; the number physicalized.
int physicalize(std::uint8_t* runtime) {
  auto** begin = *reinterpret_cast<std::uint8_t***>(runtime + kRuntimeNodes);
  auto** end = *reinterpret_cast<std::uint8_t***>(runtime + kRuntimeNodesEnd);
  int count = 0;
  for (std::uint8_t** at = begin; at && at < end; ++at) {
    std::uint8_t* node = *at;
    if (!node) {
      continue;
    }
    const char* type = rtti_name(node);
    const bool brush =
        std::strcmp(type, kBrushClass) == 0 || std::strcmp(type, kOwnedBrushClass) == 0;
    if (!brush || *reinterpret_cast<void**>(node + kBrushPhysics)) {
      continue;
    }
    kcds::game_client::slot<Physicalize>(node, kBrushPhysicalize)(node, true);
    if (*reinterpret_cast<void**>(node + kBrushPhysics)) {
      ++count;
    }
  }
  return count;
}

std::uint32_t entity_id(void* entity) {
  return kcds::game_client::slot<GetId>(entity, kEntityGetId)(entity);
}

// "x,y,z" (count 3) or "w,x,y,z" (count 4, a turn in the editor's order) into `out`; false when
// it is not that.
bool read_floats(const char* text, float* out, int count) {
  if (!text || !*text) {
    return false;
  }
  float values[4] = {};
  int read = 0;
  if (count == 3) {
    read = std::sscanf(text, "%f,%f,%f", &values[0], &values[1], &values[2]);
  } else {
    read = std::sscanf(text, "%f,%f,%f,%f", &values[0], &values[1], &values[2], &values[3]);
  }
  if (read != count) {
    return false;
  }
  for (int i = 0; i < count; ++i) {
    out[i] = values[i];
  }
  return true;
}

// Test hook (KCDS_TEST_PREFAB_HIDE_TYPES, e.g. "Decal,ClipVolume"): more object types the builder
// passes over, to find which of a template's objects crashes the game's render.
bool hidden_type(const char* type) {
  static const std::string types = [] {
    char text[256] = {};
    const DWORD length = GetEnvironmentVariableA("KCDS_TEST_PREFAB_HIDE_TYPES", text, sizeof(text));
    return length > 0 && length < sizeof(text) ? "," + std::string(text) + "," : std::string();
  }();
  return !types.empty() && types.find("," + std::string(type) + ",") != std::string::npos;
}

// While a template is built, its nested prefabs (built on their own, spawn_nested) and its decals
// are passed over. A prefab's decal (a moss or leak stain projected on its walls) crashed the
// game's render on a World Architect custom map: CDecalRenderNode drawn with a material already
// freed (the editor's content pictures of Kutna Hora's town
// houses); a game level that has the building holds the material itself.
const char* __fastcall get_attr_hiding_nested(void* node, const char* key) {
  const char* value = original_get_attr(node, key);
  if (hiding_nested && key && value && std::strcmp(key, "Type") == 0 &&
      (std::strcmp(value, "Prefab") == 0 || std::strcmp(value, "Decal") == 0 ||
       hidden_type(value))) {
    return "";
  }
  if (hiding_nested && key && value && std::strcmp(key, "EntityClass") == 0 && hidden_type(value)) {
    return "";
  }
  return value;
}

// Hooks the getAttr of the template's XML node class, once.
bool hook_get_attr(void* node) {
  void* target = (*static_cast<void***>(node))[kXmlGetAttr];
  if (hooked_get_attr_target) {
    return hooked_get_attr_target == target;
  }
  if (MH_CreateHook(target, reinterpret_cast<void*>(&get_attr_hiding_nested),
                    reinterpret_cast<void**>(&original_get_attr)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    return false;
  }
  hooked_get_attr_target = target;
  return true;
}

// Quaternions (x, y, z, w): their product, and a point turned by one (defined below).
void multiply(const float a[4], const float b[4], float out[4]);
void rotate(const float q[4], const float point[3], float out[3]);
// How many parents up a nested prefab's place is composed at most.
constexpr int kMaxParents = 16;

// Reads the prefabs the template holds (once per template) and keeps them; true when it holds
// some and the builder can be made to pass over them.
bool read_nested(const std::string& guid_text, const Guid& guid) {
  const auto known = nested_of.find(guid_text);
  if (known != nested_of.end()) {
    return !known->second.empty();
  }
  std::vector<Nested>& nested = nested_of[guid_text];
  if (!registry || !find_template) {
    return false;
  }
  std::uint8_t* found = find_template(registry(), &guid);
  void* objects = found ? *reinterpret_cast<void**>(found + kTemplateObjects) : nullptr;
  if (!objects) {
    return false;
  }
  // Every object's place in its parent (an object grouped under another, "Parent", is placed
  // relative to it, as the game's prefab items are composed), read first.
  struct Object {
    std::string id;
    std::string parent;
    std::string prefab;
    float position[3] = {};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  };
  std::vector<Object> all;
  const int count = kcds::game_client::slot<XmlChildCount>(objects, kXmlChildCount)(objects);
  const auto get_child = kcds::game_client::slot<XmlGetChild>(objects, kXmlGetChild);
  for (int index = 0; index < count; ++index) {
    void* child = nullptr;
    get_child(objects, &child, index);
    if (!child) {
      continue;
    }
    const auto attr = kcds::game_client::slot<XmlGetAttr>(child, kXmlGetAttr);
    Object object;
    const char* id = attr(child, "Id");
    const char* parent = attr(child, "Parent");
    object.id = id ? id : "";
    object.parent = parent ? parent : "";
    const char* type = attr(child, "Type");
    if (type && std::strcmp(type, "Decal") == 0 && hook_get_attr(child)) {
      decal_templates.insert(guid_text);
    }
    if (type && std::strcmp(type, "Prefab") == 0 && hook_get_attr(child)) {
      const char* inner_guid = attr(child, "PrefabTemplate");
      object.prefab = inner_guid ? inner_guid : "";
    }
    read_floats(attr(child, "Pos"), object.position, 3);
    float turn[4] = {};
    if (read_floats(attr(child, "Rotate"), turn, 4)) {
      object.rotation[0] = turn[1];
      object.rotation[1] = turn[2];
      object.rotation[2] = turn[3];
      object.rotation[3] = turn[0];
    }
    all.push_back(std::move(object));
    kcds::game_client::slot<XmlRelease>(child, kXmlRelease)(child);
  }
  for (const Object& object : all) {
    if (object.prefab.empty()) {
      continue;
    }
    Nested inner;
    inner.guid = object.prefab;
    std::copy(object.position, object.position + 3, inner.position);
    std::copy(object.rotation, object.rotation + 4, inner.rotation);
    // Up through its parents: each one's place and turn put on what it holds.
    std::string parent = object.parent;
    for (int depth = 0; !parent.empty() && depth < kMaxParents; ++depth) {
      const auto holder = std::find_if(
          all.begin(), all.end(), [&](const Object& candidate) { return candidate.id == parent; });
      if (holder == all.end()) {
        break;
      }
      float turned[3] = {};
      rotate(holder->rotation, inner.position, turned);
      for (int axis = 0; axis < 3; ++axis) {
        inner.position[axis] = holder->position[axis] + turned[axis];
      }
      float combined[4] = {};
      multiply(holder->rotation, inner.rotation, combined);
      std::copy(combined, combined + 4, inner.rotation);
      parent = holder->parent;
    }
    nested.push_back(inner);
  }
  if (log_line && !nested.empty()) {
    log_line("PREFABS template %s: %zu nested prefabs built on their own", guid_text.c_str(),
             nested.size());
  }
  return !nested.empty();
}

// a * b of quaternions (x, y, z, w).
void multiply(const float a[4], const float b[4], float out[4]) {
  out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
  out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
  out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
  out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// The point turned by the quaternion (x, y, z, w).
void rotate(const float q[4], const float point[3], float out[3]) {
  const float vector[4] = {point[0], point[1], point[2], 0.0f};
  const float conjugate[4] = {-q[0], -q[1], -q[2], q[3]};
  float half[4] = {};
  float full[4] = {};
  multiply(q, vector, half);
  multiply(half, conjugate, full);
  out[0] = full[0];
  out[1] = full[1];
  out[2] = full[2];
}

// The model an entity object of a template names in its Properties child (object_Model or
// fileModel); null for none. The text stays the template's, which lives on.
const char* entity_model(void* object) {
  const int count = kcds::game_client::slot<XmlChildCount>(object, kXmlChildCount)(object);
  const auto get_child = kcds::game_client::slot<XmlGetChild>(object, kXmlGetChild);
  const char* found = nullptr;
  for (int index = 0; index < count && !found; ++index) {
    void* child = nullptr;
    get_child(object, &child, index);
    if (!child) {
      continue;
    }
    const auto attr = kcds::game_client::slot<XmlGetAttr>(child, kXmlGetAttr);
    for (const char* key : {"object_Model", "fileModel"}) {
      const char* value = attr(child, key);
      if (!found && value && *value) {
        found = value;
      }
    }
    kcds::game_client::slot<XmlRelease>(child, kXmlRelease)(child);
  }
  return found;
}

// The brushes of a template into `out`, placed by `at` and `turn` (the holder's frame), and
// those of its nested prefabs while `depth` allows.
void collect_pieces(const std::string& guid_text, const float at[3], const float turn[4], int depth,
                    std::size_t max, std::vector<Piece>& out) {
  Guid guid;
  if (!registry || !find_template || !read_guid || !read_guid(guid_text.c_str(), &guid)) {
    return;
  }
  std::uint8_t* found = find_template(registry(), &guid);
  void* objects = found ? *reinterpret_cast<void**>(found + kTemplateObjects) : nullptr;
  if (!objects) {
    return;
  }
  const int count = kcds::game_client::slot<XmlChildCount>(objects, kXmlChildCount)(objects);
  const auto get_child = kcds::game_client::slot<XmlGetChild>(objects, kXmlGetChild);
  for (int index = 0; index < count && out.size() < max; ++index) {
    void* child = nullptr;
    get_child(objects, &child, index);
    if (!child) {
      continue;
    }
    const auto attr = kcds::game_client::slot<XmlGetAttr>(child, kXmlGetAttr);
    const char* type = attr(child, "Type");
    float position[3] = {};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    read_floats(attr(child, "Pos"), position, 3);
    float editor_turn[4] = {};
    if (read_floats(attr(child, "Rotate"), editor_turn, 4)) {
      rotation[0] = editor_turn[1];
      rotation[1] = editor_turn[2];
      rotation[2] = editor_turn[3];
      rotation[3] = editor_turn[0];
    }
    float offset[3] = {};
    rotate(turn, position, offset);
    const float placed[3] = {at[0] + offset[0], at[1] + offset[1], at[2] + offset[2]};
    float placed_turn[4] = {};
    multiply(turn, rotation, placed_turn);
    if (type && std::strcmp(type, "Brush") == 0) {
      const char* mesh = attr(child, "Prefab");
      if (mesh && *mesh) {
        Piece piece;
        piece.mesh = mesh;
        for (int axis = 0; axis < 3; ++axis) {
          piece.position[axis] = placed[axis];
        }
        for (int part = 0; part < 4; ++part) {
          piece.rotation[part] = placed_turn[part];
        }
        out.push_back(std::move(piece));
      }
    } else if (type && std::strcmp(type, "Entity") == 0) {
      // An entity's model is in its Properties (a door's object_Model, a prop's fileModel).
      const char* mesh = entity_model(child);
      if (mesh && *mesh) {
        Piece piece;
        piece.mesh = mesh;
        for (int axis = 0; axis < 3; ++axis) {
          piece.position[axis] = placed[axis];
        }
        for (int part = 0; part < 4; ++part) {
          piece.rotation[part] = placed_turn[part];
        }
        out.push_back(std::move(piece));
      }
    } else if (type && std::strcmp(type, "Prefab") == 0 && depth > 0) {
      const char* inner = attr(child, "PrefabTemplate");
      if (inner && *inner) {
        collect_pieces(inner, placed, placed_turn, depth - 1, max, out);
      }
    }
    kcds::game_client::slot<XmlRelease>(child, kXmlRelease)(child);
  }
}

bool spawn_at(const std::string& name, const std::string& guid_text, const float position[3],
              const float rotation[4], int depth);

// Builds the template's nested prefabs in the holder's frame, named after it.
void spawn_nested(const std::string& name, const std::string& guid_text, const float position[3],
                  const float rotation[4], int depth) {
  const auto found = nested_of.find(guid_text);
  if (found == nested_of.end() || depth >= kMaxNesting) {
    return;
  }
  std::vector<std::string>& names = inner_names[name];
  for (std::size_t index = 0; index < found->second.size(); ++index) {
    const Nested& inner = found->second[index];
    float offset[3] = {};
    rotate(rotation, inner.position, offset);
    const float at[3] = {position[0] + offset[0], position[1] + offset[1], position[2] + offset[2]};
    float turn[4] = {};
    multiply(rotation, inner.rotation, turn);
    const std::string inner_name = name + "_" + std::to_string(index);
    if (spawn_at(inner_name, inner.guid, at, turn, depth + 1)) {
      names.push_back(inner_name);
    }
  }
}

}  // namespace

bool install(Logger logger) {
  log_line = logger;
  read_guid = reinterpret_cast<ReadGuid>(
      resolve_unique("PrefabGuidReader", kSpawnBinding, kGuidReaderCall, kGuidReaderEnd));
  spawner = reinterpret_cast<Spawner>(
      resolve_unique("PrefabSpawner", kSpawnBinding, kSpawnerCall, kSpawnerEnd));
  game_global = reinterpret_cast<std::uint8_t**>(
      resolve_unique("PrefabGameGlobal", kSpawnBinding, kGlobalLoad, kGlobalLoadEnd));
  delete_prefab = reinterpret_cast<Delete>(
      resolve_unique("PrefabDelete", kDeleteBinding, kDeleteCall, kDeleteEnd));
  if (!spawner) {
    return false;
  }
  builder = reinterpret_cast<Builder>(
      call_target(reinterpret_cast<std::uint8_t*>(spawner) + kBuilderCall));
  find_runtime = reinterpret_cast<FindRuntime>(
      call_target(reinterpret_cast<std::uint8_t*>(spawner) + kFindRuntimeCall));
  if (builder) {
    auto* start = reinterpret_cast<std::uint8_t*>(builder);
    registry = reinterpret_cast<Registry>(call_target(start + kRegistryCall));
    find_template = reinterpret_cast<FindTemplate>(call_target(start + kFindTemplateCall));
  }
  const bool found = read_guid && spawner && game_global && delete_prefab && builder &&
                     find_runtime && registry && find_template;
  if (log_line) {
    log_line("PREFABS game prefabs %s", found ? "found" : "not found");
  }
  return found;
}

std::string template_of(const std::string& model) {
  constexpr std::size_t kPrefixLength = sizeof(kModelPrefix) - 1;
  if (model.compare(0, kPrefixLength, kModelPrefix) != 0) {
    return std::string();
  }
  return model.substr(kPrefixLength);
}

namespace {

bool spawn_at(const std::string& name, const std::string& guid_text, const float position[3],
              const float rotation[4], int depth) {
  void* prefab_manager = manager();
  Guid guid;
  if (!prefab_manager || !builder || !read_guid(guid_text.c_str(), &guid)) {
    return false;
  }
  hiding_nested = read_nested(guid_text, guid) || decal_templates.count(guid_text) != 0;
  std::uint8_t* built = builder(prefab_manager, &guid);
  hiding_nested = false;
  if (!built) {
    if (log_line) {
      log_line("PREFABS %s: template %s unknown or not buildable", name.c_str(), guid_text.c_str());
    }
    return false;
  }
  const int fixed = fix_saturated_flags(built);
  name_entities(built, name);
  entity::remove(name);
  void* holder = entity::spawn_class_with(name, kPrefabClass, position, rotation, {});
  if (!holder) {
    return false;
  }
  const std::uint32_t id = entity_id(holder);
  spawner(prefab_manager, &guid, id, 0, true);
  std::uint8_t* runtime = runtime_of(prefab_manager, id);
  const int solid = runtime ? physicalize(runtime) : 0;
  if (log_line) {
    log_line(
        "PREFABS %s: template %s built at %.1f %.1f %.1f turn %.2f %.2f %.2f %.2f (entity %u, "
        "%d brushes solid, %d flags fixed)",
        name.c_str(), guid_text.c_str(), position[0], position[1], position[2], rotation[0],
        rotation[1], rotation[2], rotation[3], id, solid, fixed);
  }
  if (runtime) {
    spawn_nested(name, guid_text, position, rotation, depth);
  }
  return runtime != nullptr;
}

}  // namespace

bool spawn(const std::string& name, const std::string& guid, const float position[3],
           const float rotation[4]) {
  return spawn_at(name, guid, position, rotation, 0);
}

void remove(const std::string& name) {
  const auto inner = inner_names.find(name);
  if (inner != inner_names.end()) {
    const std::vector<std::string> names = inner->second;
    inner_names.erase(inner);
    for (const std::string& inner_name : names) {
      remove(inner_name);
    }
  }
  void* holder = entity::find(name);
  void* prefab_manager = manager();
  if (holder && prefab_manager && delete_prefab) {
    delete_prefab(prefab_manager, entity_id(holder));
  }
  entity::remove(name);
}

std::vector<Piece> pieces(const std::string& guid, std::size_t max) {
  std::vector<Piece> out;
  const float at[3] = {0.0f, 0.0f, 0.0f};
  const float turn[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  // As deep as the spawner builds nested prefabs: a castle made of nested prefabs only (its
  // parts in nested prefabs inside nested prefabs) has its pieces there.
  collect_pieces(guid, at, turn, kMaxNesting, max, out);
  return out;
}

}  // namespace kcds::runtime::prefabs
