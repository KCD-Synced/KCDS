#include "level_edits.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "../props/prop_entity.hpp"
#include "model_parts.hpp"
#include "story_layers.hpp"

namespace kcds::runtime::level {
namespace {

using kcd2s::integration::LevelEdit;
using kcd2s::integration::LevelEditChange;
using kcd2s::integration::LevelEditKind;

// IEntitySystem: the entity iterator (slot 22; AddRef 1, Release 2, Next 4, MoveFirst 6). CEntity:
// its key, a 64-bit EntityGuid (slot 2); SetPos (39), GetPos (40), SetRotation (41), GetRotation
// (42), SetScale (43), GetScale (44), Hide (63).
constexpr std::size_t kIteratorSlot = 22;
constexpr std::size_t kAddRef = 1;
constexpr std::size_t kRelease = 2;
constexpr std::size_t kNext = 4;
constexpr std::size_t kMoveFirst = 6;
constexpr std::size_t kEntityKey = 2;
constexpr std::size_t kSetPos = 39;
constexpr std::size_t kGetPos = 40;
constexpr std::size_t kSetRotation = 41;
constexpr std::size_t kGetRotation = 42;
constexpr std::size_t kSetScale = 43;
constexpr std::size_t kGetScale = 44;
constexpr std::size_t kEntityHide = 63;

// IPhysicalWorld: GetEntitiesInBox(min, max, list, types, preallocated) (slot 34). Static
// entities only (ent_static).
constexpr std::size_t kEntitiesInBox = 34;
constexpr int kStaticEntities = 1;
// I3DEngine: GetObjectsInBox(box (min, max), nodes or null) -> count (slot 243). A box holds no
// more render nodes than this for an edit.
constexpr std::size_t kObjectsInBox = 243;
constexpr std::uint32_t kMaxRenderNodes = 20000;
// IPhysicalEntity: GetForeignData(type) (slot 10) and the type it holds (slot 11).
constexpr std::size_t kForeignData = 10;
constexpr std::size_t kForeignType = 11;
constexpr int kForeignStatic = 1;
// IPhysicalEntity::GetStatus (slot 6) with pe_status_pos (type 1; partid + 4, ipart + 8 and
// flagsAND + 20 unused as -1): its place at + 24 and its bounds around it from + 36 (min, max).
constexpr std::size_t kGetStatus = 6;
constexpr int kStatusPos = 1;
constexpr std::size_t kStatusSize = 256;
constexpr std::size_t kStatusPlace = 24;
constexpr std::size_t kStatusBounds = 36;
// IPhysicalWorld::DestroyPhysicalEntity(entity, mode, thread safe) (slot 19): mode 1
// suspends the entity (out of the world's grid, kept on the world's suspended list at + 752,
// linked through the entity's + 88), mode 2 restores it.
constexpr std::size_t kDestroyEntity = 19;
constexpr int kSuspend = 1;
constexpr int kRestore = 2;
constexpr std::size_t kSuspendedList = 752;
constexpr std::size_t kSuspendedNext = 88;
constexpr int kMaxSuspended = 100000;
// The level's own colliders of a building are static entities without foreign data standing at
// exactly the place of the building's brushes (KCD2 villages: the outside, its inside and its
// distant copy share one place and none has physics of its own).
constexpr float kSamePlace = 0.01f;
// CBrush's mesh (CStatObj, + 152): it has physics geometry when its list at + 296 (to + 304) holds
// one or its flags at + 376 have 0x20 (as CBrush::Physicalize, checks).
constexpr std::size_t kBrushMesh = 152;
constexpr std::size_t kMeshPhysicsBegin = 296;
constexpr std::size_t kMeshPhysicsEnd = 304;
constexpr std::size_t kMeshFlags = 376;
constexpr std::uint8_t kMeshPhysicsFlag = 0x20;

// CBrush (an IRenderNode): its RTTI name, GetName (slot 9, the mesh path), SetMatrix (16),
// Physicalize(instant) (33), Dephysicalize(keep if referenced) (48), Hide (68); its world matrix
// (3x4, rows) at + 80, its render flags at + 40, ERF_HIDDEN 0x100, and its world bounds at + 180
// (min) and + 192 (max).
constexpr char kBrushClass[] = ".?AVCBrush@@";
constexpr std::size_t kBrushName = 9;
constexpr std::size_t kBrushSetMatrix = 16;
constexpr std::size_t kBrushPhysicalize = 33;
constexpr std::size_t kBrushDephysicalize = 48;
// A brush's own physical entity (+ 128). IPhysicalWorld::DestroyPhysicalEntity(entity, mode, thread
// safe) (slot 19): mode 1 suspends the entity (kept on the world's suspended list at
// + 752, linked through the entity's + 88), mode 2 restores it with all its parts.
constexpr std::size_t kBrushPhysics = 128;
constexpr std::size_t kBrushHide = 68;
// CBrush SetMaterial (slot 37; the material drawn in place of its mesh's own, a smart pointer at
// + 136, null to draw its mesh's own again) and that override.
constexpr std::size_t kBrushSetMaterial = 37;
constexpr std::size_t kBrushGetMaterial = 38;
constexpr std::size_t kBrushMaterial = 136;
// IMaterial GetName (slot 8).
constexpr std::size_t kMaterialName = 8;
// CEntity SetMaterial (slot 84) and GetMaterial (slot 85, its override or null).
constexpr std::size_t kEntitySetMaterial = 84;
constexpr std::size_t kEntityGetMaterial = 85;
constexpr std::size_t kBrushMatrix = 80;
constexpr std::size_t kBrushFlags = 40;
constexpr std::size_t kBrushBoundsMin = 180;
constexpr std::size_t kBrushBoundsMax = 192;
// Its level layer's id (leveldata.xml's Layer Id), as CBrush's GetLayerId (slot 44).
constexpr std::size_t kBrushLayer = 0x92;
constexpr std::uint64_t kHiddenFlag = 0x100;
// CVegetation (an IRenderNode): the same slots and flags as CBrush; its matrix is
// built by CalcMatrix (Natives::vegetation_matrix).
constexpr char kVegetationClass[] = ".?AVCVegetation@@";
// IRenderNode OffsetPosition(delta) (slot 20; CVegetation's takes its box out of the
// on-demand physics grid and puts it back where it now stands).
constexpr std::size_t kNodeOffsetPosition = 20;

// A brush's place is looked for within this box (metres each way) around where it should be.
constexpr float kSearchReach = 2.0f;
// The engine's tree is searched by the brushes' bounds, which may start metres from the brush's
// place (the Malesov drawbridge's deck begins well out from its hinge).
constexpr float kRenderNodeReach = 10.0f;
constexpr ULONGLONG kRetryMilliseconds = 1000;
// The most new edits that look for their objects in one frame.
constexpr int kFirstAppliesPerFrame = 64;
// The level streams objects out and in again (a tree the player walked away from comes back as a
// new render node): an applied brush edit looks for its copies again this often, and at most this
// many edits do so in one frame.
constexpr ULONGLONG kRecheckMilliseconds = 2000;
// A plant's edit looks for its copy less often: World Architect's Clear nature makes thousands, and
// looking for them all every 2 s, each a search of the level, held the game at 10 fps.
constexpr ULONGLONG kPlantRecheckMilliseconds = 10000;
// The time the edits may take of a frame: looking for their copies again, and new edits looking
// for their objects (each a search of the level, a few milliseconds).
constexpr double kRecheckBudgetMs = 2.0;
constexpr double kFirstApplyBudgetMs = 4.0;
// How often the edits' state is logged, milliseconds.
constexpr ULONGLONG kStatusMilliseconds = 10000;

using Get = void*(__fastcall*)(void*);
using Call = void(__fastcall*)(void*);
using EntityKey = std::uint64_t(__fastcall*)(void*);
using GetVector = const float*(__fastcall*)(void*);
using SetPos = void(__fastcall*)(void*, const float*, int, bool, bool);
using SetRotation = void(__fastcall*)(void*, const float*, int);
using SetScale = void(__fastcall*)(void*, const float*, int);
using EntityHide = void(__fastcall*)(void*, bool);
using EntitiesInBox = int(__fastcall*)(void* world, const float* min, const float* max,
                                       void*** list, int types, int preallocated);
using ObjectsInBox = std::uint32_t(__fastcall*)(void* engine, const float* box, void** nodes);
using ForeignData = void*(__fastcall*)(void* entity, int type);
using ForeignType = int(__fastcall*)(void* entity);
using BrushName = const char*(__fastcall*)(void* brush);
using BrushSetMatrix = void(__fastcall*)(void* brush, const float* matrix);
using BrushPhysicalize = void(__fastcall*)(void* brush, bool instant);
using BrushDephysicalize = void(__fastcall*)(void* brush, bool keep);
using BrushHide = void(__fastcall*)(void* brush, bool hide);
using SetMaterial = void(__fastcall*)(void* object, void* material);
using MaterialName = const char*(__fastcall*)(void* material);
using GetStatus = int(__fastcall*)(void* entity, void* status);
using DestroyEntity = int(__fastcall*)(void* world, void* entity, int mode, int thread_safe);
using NodeOffsetPosition = void(__fastcall*)(void* node, const float* delta);

Natives natives;
Logger log_line = nullptr;
bool installed = false;

// The vegetation instances edits hide. The engine's on-demand physics physicalizes a
// place's vegetation again when a player comes back to it; CVegetation::Physicalize is hooked to
// refuse these, so a hidden tree never stands as an invisible wall. The hook may run off the
// engine thread.
using VegetationPhysicalize = void(__fastcall*)(void* vegetation, bool instant);
VegetationPhysicalize original_vegetation_physicalize = nullptr;
std::mutex kept_out_mutex;
std::unordered_set<void*> kept_out;

void __fastcall hooked_vegetation_physicalize(void* vegetation, bool instant) {
  {
    std::lock_guard<std::mutex> lock(kept_out_mutex);
    if (kept_out.count(vegetation) != 0) {
      return;
    }
  }
  original_vegetation_physicalize(vegetation, instant);
}

void keep_out(void* vegetation, bool out) {
  std::lock_guard<std::mutex> lock(kept_out_mutex);
  if (out) {
    kept_out.insert(vegetation);
  } else {
    kept_out.erase(vegetation);
  }
}

// One copy of an edit's brush. The level may hold the same brush several times at one place, one
// per layer (the Suchdol portcullis's raised bars five times, one shown); an edit takes them all.
struct BrushCopy {
  void* brush = nullptr;
  float matrix[12] = {};      // the level's own
  bool level_hidden = false;  // its layer hides it: shown or not, it stays as the level has it
  bool hidden = false;        // what this game did to it
  bool moved = false;
  bool dephysicalized = false;
  void* suspended = nullptr;  // the brush's own physical entity, suspended while it is hidden
  // The level's own colliders of its building, suspended while the brush is hidden.
  std::vector<void*> colliders;
  bool vegetation = false;  // a CVegetation (kept out of the on-demand physics while hidden)
  // The material the level drew it with in place of its mesh's own (a reference held here, so it
  // can be put back after another replaced it), and the one this game draws it with now ("" for
  // the level's).
  void* level_material = nullptr;
  std::string material;
  bool material_refused = false;  // told once that a plant keeps its own material
  // What the level drew it with (its override, else its mesh's own), and the copy of a material
  // with parts hidden it is drawn with (its reference held here) for the parts named in `parts`.
  void* own_material = nullptr;
  void* parts_material = nullptr;
  std::string parts;
};

// One edit as this game has it: what the server wants, the object once found and how the level
// had it before the first change.
struct Applied {
  LevelEdit edit;
  void* object = nullptr;  // the CEntity, or the first brush copy (the edit is found)
  std::vector<BrushCopy> copies;
  bool captured = false;
  float position[3] = {};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float scale[3] = {1.0f, 1.0f, 1.0f};
  // What this game did to it.
  bool hidden = false;
  bool moved = false;
  // An entity's material as the level had it (its override, almost always none), and the one this
  // game draws it with now.
  void* level_material = nullptr;
  std::string material;
  int misses = 0;      // how often its brush was looked for and not found
  bool tried = false;  // looked for at least once
  ULONGLONG next_try = 0;
  ULONGLONG next_check = 0;  // when an applied brush edit looks for its copies again
};

std::map<std::uint32_t, Applied> edits;

// The layer edits: a story layer switched on or off (story_layers.hpp). One the game does not
// have yet (the level still loading it) is tried again every kRetryMilliseconds.
struct LayerEdit {
  std::string name;
  bool on = false;
  bool applied = false;
  ULONGLONG next_try = 0;
};
std::map<std::uint32_t, LayerEdit> layer_edits;

// Where the level had the objects (not vegetation) the edits hid or moved: their smart objects are
// not to be used (blocks_smart_object). Rebuilt when the edits change; read on any thread.
// A bench's place in the level is often at one of its ends: its seats lie up to 2 m away.
constexpr float kSmartObjectReach = 2.5f;
std::mutex blocked_mutex;
std::vector<std::array<float, 3>> blocked_places;

bool is_vegetation(const std::string& name) {
  return name.find("/vegetation/") != std::string::npos ||
         name.find("objects/natural/") != std::string::npos;
}

void rebuild_blocked_places() {
  std::vector<std::array<float, 3>> places;
  for (const auto& [id, applied] : edits) {
    const LevelEdit& wanted = applied.edit;
    if (!(wanted.hidden || wanted.has_position) || is_vegetation(wanted.name)) {
      continue;
    }
    places.push_back({wanted.at[0], wanted.at[1], wanted.at[2]});
  }
  std::lock_guard<std::mutex> lock(blocked_mutex);
  blocked_places = std::move(places);
}
// A missed edit names what stands near it in the log: a few lines each, for the first few misses.
// Every object near every missed edit was named (224 743 lines for World Architect's 2 601 hidden
// plants, the frames held while they were written and newer edits never applied).
constexpr int kMostToldPerMiss = 6;
constexpr int kMostMissesTold = 20;
int tell_left = 0;
int misses_told = 0;
// Where the edits still looking for their objects take turns again next frame (see frame()).
std::uint32_t resume_after = 0;

template <class T>
T read(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
  return value;
}

template <class Function>
Function slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}

void say(const char* format, std::uint32_t id, const char* what) {
  if (log_line) {
    log_line(format, id, what);
  }
}

// The object's RTTI class name (".?AV...@@"), empty when it has none.
const char* rtti_name(void* object) {
  void** vtable = *static_cast<void***>(object);
  const std::uint8_t* locator = static_cast<const std::uint8_t*>(vtable[-1]);
  if (!locator) {
    return "";
  }
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"WHGame.dll"));
  const std::uintptr_t descriptor = base + read<std::uint32_t>(locator, 12);
  return reinterpret_cast<const char*>(descriptor + 16);
}

bool rtti_is(void* object, const char* name) {
  return std::strcmp(rtti_name(object), name) == 0;
}

// Mesh paths compare without case and with either slash.
bool same_mesh(const char* a, const std::string& b) {
  std::size_t i = 0;
  for (; a[i] != '\0' && i < b.size(); ++i) {
    char x = char(std::tolower(static_cast<unsigned char>(a[i])));
    char y = char(std::tolower(static_cast<unsigned char>(b[i])));
    x = x == '\\' ? '/' : x;
    y = y == '\\' ? '/' : y;
    if (x != y) {
      return false;
    }
  }
  return a[i] == '\0' && i == b.size();
}

bool within_reach(const float a[3], const float b[3], float reach) {
  float sum = 0.0f;
  for (int axis = 0; axis < 3; ++axis) {
    sum += (a[axis] - b[axis]) * (a[axis] - b[axis]);
  }
  return sum <= reach * reach;
}

// ---- Finding the object ------------------------------------------------------------------------

// Whether the render node is a vegetation instance whose matrix can be built.
bool is_vegetation(void* node) {
  return natives.vegetation_matrix != nullptr && rtti_is(node, kVegetationClass);
}

// The world matrix (3x4 by rows) of a brush or a vegetation instance; false for anything else.
bool node_matrix(void* node, float matrix[12]) {
  if (rtti_is(node, kBrushClass)) {
    std::memcpy(matrix, static_cast<std::uint8_t*>(node) + kBrushMatrix, sizeof(float) * 12);
    return true;
  }
  if (is_vegetation(node)) {
    // CalcMatrix stores the rows with aligned moves.
    alignas(16) float built[12];
    natives.vegetation_matrix(node, built);
    std::memcpy(matrix, built, sizeof(built));
    return true;
  }
  return false;
}

// Whether the render node is a brush (or a vegetation instance) of the edit's mesh within the
// match distance of `where`. `tell`: its mesh and place are logged (an edit whose brush is not
// found).
bool is_edit_brush(void* node, const LevelEdit& edit, const float where[3], bool tell) {
  float matrix[12];
  if (!node_matrix(node, matrix)) {
    return false;
  }
  const char* mesh = slot_of<BrushName>(node, kBrushName)(node);
  const float place[3] = {matrix[3], matrix[7], matrix[11]};
  if (tell && log_line && tell_left > 0) {
    --tell_left;
    log_line("LEVEL edit %u: near it %s at %.3f,%.3f,%.3f layer %u%s", edit.id, mesh ? mesh : "?",
             place[0], place[1], place[2], unsigned(read<std::uint16_t>(node, kBrushLayer)),
             (read<std::uint64_t>(node, kBrushFlags) & kHiddenFlag) != 0 ? " hidden" : "");
  }
  return mesh && same_mesh(mesh, edit.name) &&
         within_reach(place, where, kcd2s::integration::kLevelEditMatchMetres);
}

void add_once(std::vector<void*>& found, void* brush) {
  for (void* known : found) {
    if (known == brush) {
      return;
    }
  }
  found.push_back(brush);
}

// ---- The level's own colliders ----------------------------------------------------------------

// A static entity's place and its bounds in the world (min, max); false when it gives none.
bool collider_place(void* entity, float place[3], float bounds[6]) {
  alignas(16) std::uint8_t status[kStatusSize] = {};
  const int unused = -1;
  std::memcpy(status, &kStatusPos, sizeof(kStatusPos));
  std::memcpy(status + 4, &unused, sizeof(unused));
  std::memcpy(status + 8, &unused, sizeof(unused));
  std::memcpy(status + 20, &unused, sizeof(unused));
  if (!slot_of<GetStatus>(entity, kGetStatus)(entity, status)) {
    return false;
  }
  std::memcpy(place, status + kStatusPlace, sizeof(float) * 3);
  float around[6];
  std::memcpy(around, status + kStatusBounds, sizeof(around));
  for (int axis = 0; axis < 3; ++axis) {
    bounds[axis] = place[axis] + around[axis];
    bounds[axis + 3] = place[axis] + around[axis + 3];
  }
  return true;
}

bool same_place(const float a[3], const float b[3]) {
  return std::fabs(a[0] - b[0]) < kSamePlace && std::fabs(a[1] - b[1]) < kSamePlace &&
         std::fabs(a[2] - b[2]) < kSamePlace;
}

// The volume of a brush's world bounds.
float bounds_volume(void* brush) {
  float volume = 1.0f;
  for (int axis = 0; axis < 3; ++axis) {
    volume *= read<float>(brush, kBrushBoundsMax + axis * 4) -
              read<float>(brush, kBrushBoundsMin + axis * 4);
  }
  return volume;
}

bool has_physics_geometry(void* brush) {
  void* mesh = read<void*>(brush, kBrushMesh);
  if (!mesh) {
    return false;
  }
  const auto begin = read<std::uintptr_t>(mesh, kMeshPhysicsBegin);
  const auto end = read<std::uintptr_t>(mesh, kMeshPhysicsEnd);
  return end > begin || (read<std::uint8_t>(mesh, kMeshFlags) & kMeshPhysicsFlag) != 0;
}

// The building a level collider stands for: of the brushes at exactly its place, one with physics
// geometry before one without, then the largest (the outside before its inside).
void* building_at(const float place[3]) {
  void* best = nullptr;
  bool best_physics = false;
  float best_volume = 0.0f;
  for (const LevelBrush& brush : brushes_near(place, 1.0f)) {
    const float at[3] = {brush.matrix[3], brush.matrix[7], brush.matrix[11]};
    if (!same_place(at, place)) {
      continue;
    }
    const bool physics = has_physics_geometry(brush.brush);
    const float volume = bounds_volume(brush.brush);
    const bool better =
        !best || (physics && !best_physics) || (physics == best_physics && volume > best_volume);
    if (better) {
      best = brush.brush;
      best_physics = physics;
      best_volume = volume;
    }
  }
  return best;
}

// The level's own colliders standing at exactly a place: static entities without foreign data. A
// hidden or moved brush takes those of its level place with it, so hiding a building takes all of
// its collision and moving one leaves no invisible walls behind.
std::vector<void*> level_colliders_at(const float at[3]) {
  std::vector<void*> found;
  void* world = natives.physical_world ? *natives.physical_world : nullptr;
  if (!world) {
    return found;
  }
  const float min[3] = {at[0] - 0.5f, at[1] - 0.5f, at[2] - 0.5f};
  const float max[3] = {at[0] + 0.5f, at[1] + 0.5f, at[2] + 0.5f};
  void** list = nullptr;
  const int count =
      slot_of<EntitiesInBox>(world, kEntitiesInBox)(world, min, max, &list, kStaticEntities, 0);
  std::vector<void*> candidates;
  for (int index = 0; list && index < count; ++index) {
    void* physical = list[index];
    if (!physical || slot_of<ForeignType>(physical, kForeignType)(physical) != kForeignStatic ||
        slot_of<ForeignData>(physical, kForeignData)(physical, kForeignStatic) != nullptr) {
      continue;
    }
    candidates.push_back(physical);
  }
  for (void* physical : candidates) {
    float place[3];
    float bounds[6];
    if (collider_place(physical, place, bounds) && same_place(place, at)) {
      found.push_back(physical);
    }
  }
  return found;
}

// Whether the world still keeps this entity suspended (it was not destroyed meanwhile).
bool still_suspended(void* world, void* entity) {
  void* each = read<void*>(world, kSuspendedList);
  for (int count = 0; each && count < kMaxSuspended; ++count) {
    if (each == entity) {
      return true;
    }
    each = read<void*>(each, kSuspendedNext);
  }
  return false;
}

void suspend_colliders(BrushCopy& copy) {
  void* world = natives.physical_world ? *natives.physical_world : nullptr;
  if (!world) {
    return;
  }
  // Where the level placed the brush: its own matrix while it is moved, else where it stands.
  float at[3] = {read<float>(copy.brush, kBrushMatrix + 12),
                 read<float>(copy.brush, kBrushMatrix + 28),
                 read<float>(copy.brush, kBrushMatrix + 44)};
  if (copy.moved) {
    at[0] = copy.matrix[3];
    at[1] = copy.matrix[7];
    at[2] = copy.matrix[11];
  }
  copy.colliders = level_colliders_at(at);
  for (void* collider : copy.colliders) {
    slot_of<DestroyEntity>(world, kDestroyEntity)(world, collider, kSuspend, 0);
  }
  if (log_line && !copy.colliders.empty()) {
    log_line("LEVEL EDIT a brush took %zu of the level's colliders with it", copy.colliders.size());
  }
}

void restore_colliders(BrushCopy& copy) {
  void* world = natives.physical_world ? *natives.physical_world : nullptr;
  for (void* collider : copy.colliders) {
    if (world && still_suspended(world, collider)) {
      slot_of<DestroyEntity>(world, kDestroyEntity)(world, collider, kRestore, 0);
    }
  }
  copy.colliders.clear();
}

// The level's colliders of a copy stay out while it is hidden or moved away from them.
void sync_colliders(BrushCopy& copy) {
  const bool out = copy.dephysicalized || copy.moved;
  if (out && copy.colliders.empty()) {
    suspend_colliders(copy);
  } else if (!out && !copy.colliders.empty()) {
    restore_colliders(copy);
  }
}

// The brushes among the 3D engine's render nodes in the box around `where` (those without physics
// and those their layer hides included).
void find_render_nodes(const LevelEdit& edit, const float where[3], bool tell,
                       std::vector<void*>& found) {
  void* engine = natives.engine_3d ? *natives.engine_3d : nullptr;
  if (!engine) {
    return;
  }
  const float box[6] = {where[0] - kRenderNodeReach, where[1] - kRenderNodeReach,
                        where[2] - kRenderNodeReach, where[0] + kRenderNodeReach,
                        where[1] + kRenderNodeReach, where[2] + kRenderNodeReach};
  const auto objects_in_box = slot_of<ObjectsInBox>(engine, kObjectsInBox);
  const std::uint32_t count = objects_in_box(engine, box, nullptr);
  if (count == 0 || count > kMaxRenderNodes) {
    return;
  }
  std::vector<void*> nodes(count + 16, nullptr);
  const std::uint32_t filled = objects_in_box(engine, box, nodes.data());
  for (std::uint32_t index = 0; index < filled && index < nodes.size(); ++index) {
    if (nodes[index] && is_edit_brush(nodes[index], edit, where, tell)) {
      add_once(found, nodes[index]);
    }
  }
}

// Every copy of the edit's brush standing within the match distance of `where`, found through
// their physics and the engine's object tree. `tell`: the brushes searched are logged.
std::vector<void*> find_brushes(const LevelEdit& edit, const float where[3], bool tell) {
  std::vector<void*> found;
  if (tell) {
    // A plant's edit (Clear nature makes thousands) is not told.
    tell = misses_told < kMostMissesTold && edit.name.find("/vegetation/") == std::string::npos;
    misses_told += tell ? 1 : 0;
    tell_left = tell ? kMostToldPerMiss : 0;
  }
  void* world = natives.physical_world ? *natives.physical_world : nullptr;
  if (!world) {
    return found;
  }
  const float min[3] = {where[0] - kSearchReach, where[1] - kSearchReach, where[2] - kSearchReach};
  const float max[3] = {where[0] + kSearchReach, where[1] + kSearchReach, where[2] + kSearchReach};
  void** list = nullptr;
  const int count =
      slot_of<EntitiesInBox>(world, kEntitiesInBox)(world, min, max, &list, kStaticEntities, 0);
  for (int index = 0; list && index < count; ++index) {
    void* physical = list[index];
    if (!physical || slot_of<ForeignType>(physical, kForeignType)(physical) != kForeignStatic) {
      continue;
    }
    void* brush = slot_of<ForeignData>(physical, kForeignData)(physical, kForeignStatic);
    if (brush && is_edit_brush(brush, edit, where, tell)) {
      add_once(found, brush);
    }
  }
  find_render_nodes(edit, where, tell, found);
  return found;
}

// Finds the level entities of the edits that have none yet, in one walk of the entity system.
void find_entities() {
  std::map<std::uint64_t, Applied*> wanted;
  for (auto& [id, applied] : edits) {
    if (applied.edit.kind == LevelEditKind::Entity && !applied.object) {
      wanted[applied.edit.guid] = &applied;
    }
  }
  void* system = natives.entity_system ? natives.entity_system() : nullptr;
  void* iterator =
      system && !wanted.empty() ? slot_of<Get>(system, kIteratorSlot)(system) : nullptr;
  if (!iterator) {
    return;
  }
  slot_of<Call>(iterator, kAddRef)(iterator);
  slot_of<Call>(iterator, kMoveFirst)(iterator);
  while (void* entity = slot_of<Get>(iterator, kNext)(iterator)) {
    const auto found = wanted.find(slot_of<EntityKey>(entity, kEntityKey)(entity));
    if (found != wanted.end()) {
      found->second->object = entity;
    }
  }
  slot_of<Call>(iterator, kRelease)(iterator);
}

// ---- Changing it -------------------------------------------------------------------------------

void capture(Applied& applied) {
  if (applied.captured || applied.edit.kind != LevelEditKind::Entity) {
    return;
  }
  void* object = applied.object;
  std::memcpy(applied.position, slot_of<GetVector>(object, kGetPos)(object), sizeof(float) * 3);
  std::memcpy(applied.rotation, slot_of<GetVector>(object, kGetRotation)(object),
              sizeof(float) * 4);
  std::memcpy(applied.scale, slot_of<GetVector>(object, kGetScale)(object), sizeof(float) * 3);
  applied.level_material = slot_of<Get>(object, kEntityGetMaterial)(object);
  applied.captured = true;
}

// How the level has a brush copy before the first change: its matrix and whether its layer hides
// it.
BrushCopy capture_copy(void* brush) {
  BrushCopy copy;
  copy.brush = brush;
  node_matrix(brush, copy.matrix);
  copy.level_hidden = (read<std::uint64_t>(brush, kBrushFlags) & kHiddenFlag) != 0;
  copy.hidden = copy.level_hidden;
  copy.vegetation = is_vegetation(brush);
  if (!copy.vegetation) {
    copy.level_material = read<void*>(brush, kBrushMaterial);
    parts::hold(copy.level_material);
    // GetMaterial reads the override, else its mesh's own.
    copy.own_material = slot_of<Get>(brush, kBrushGetMaterial)(brush);
  }
  return copy;
}

// The material an object is to be drawn with: the edit's, or for "" (and a material the game
// does not have) the level's own. False when there is nothing to set: the level drew it with an
// override of its own, which this game does not hold a reference to and so does not replace.
bool material_to_draw(const std::string& wanted, void* level_material, void*& material) {
  if (level_material) {
    return false;
  }
  material = wanted.empty() ? nullptr : kcds::runtime::props::entity::load_material(wanted);
  return true;
}

std::string parts_key(const std::vector<std::string>& hidden) {
  std::string key;
  for (const std::string& part : hidden) {
    key += part;
    key += '\n';
  }
  return key;
}

// Draws a brush copy with the edit's material and without its hidden parts, or as the level has it
// again.
void draw_brush(BrushCopy& copy, const std::string& wanted,
                const std::vector<std::string>& hidden) {
  const std::string key = parts_key(hidden);
  if (wanted == copy.material && key == copy.parts) {
    return;
  }
  // A plant (CVegetation) keeps its material elsewhere than a brush does: it keeps its own.
  if (copy.vegetation) {
    if (log_line && !copy.material_refused) {
      log_line("LEVEL edit material %s: a plant keeps its own material", wanted.c_str());
    }
    copy.material_refused = true;
    return;
  }
  void* material = wanted.empty() ? nullptr : kcds::runtime::props::entity::load_material(wanted);
  void* with_parts = nullptr;
  if (!hidden.empty()) {
    void* source = material ? material : copy.own_material;
    std::string names;
    int matched = 0;
    with_parts = parts::without(source, hidden, names, matched);
    if (log_line) {
      if (!with_parts) {
        log_line("LEVEL edit parts: %s has no parts to hide, drawn whole", rtti_name(copy.brush));
      } else {
        log_line("LEVEL edit parts: %d of its parts hidden (its parts: %s)", matched,
                 names.c_str());
      }
    }
  }
  // Neither: the level's own again (its override, or none for its mesh's).
  void* drawn = with_parts ? with_parts : (material ? material : copy.level_material);
  slot_of<SetMaterial>(copy.brush, kBrushSetMaterial)(copy.brush, drawn);
  // The brush holds its own reference to the copy it now draws with.
  parts::release(copy.parts_material);
  copy.parts_material = with_parts;
  copy.parts = key;
  copy.material = material ? wanted : std::string();
  if (log_line && !wanted.empty()) {
    // What the brush draws with now, as GetMaterial reads it (its override, else its mesh's).
    void* now = slot_of<Get>(copy.brush, kBrushGetMaterial)(copy.brush);
    const char* now_name = now ? slot_of<MaterialName>(now, kMaterialName)(now) : nullptr;
    log_line("LEVEL edit material %s %s (the brush draws %s)", wanted.c_str(),
             material ? "set" : "is not one of the game's: the level's own stays",
             now_name ? now_name : "?");
  }
}

// The brush's matrix for the edit: the level's own, with the edit's place, turn and size.
void brush_matrix(const LevelEdit& edit, const float level[12], float out[12]) {
  float axes[3][3];  // axes[column][row], unit length
  float sizes[3];
  for (int column = 0; column < 3; ++column) {
    const float x = level[column];
    const float y = level[4 + column];
    const float z = level[8 + column];
    sizes[column] = std::sqrt(x * x + y * y + z * z);
    const float length = sizes[column] > 0.0f ? sizes[column] : 1.0f;
    axes[column][0] = x / length;
    axes[column][1] = y / length;
    axes[column][2] = z / length;
  }
  if (edit.has_rotation) {
    float x = edit.rotation[0];
    float y = edit.rotation[1];
    float z = edit.rotation[2];
    float w = edit.rotation[3];
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    if (length > 0.0f) {
      x /= length;
      y /= length;
      z /= length;
      w /= length;
    }
    const float rows[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
        {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
        {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)},
    };
    for (int column = 0; column < 3; ++column) {
      for (int row = 0; row < 3; ++row) {
        axes[column][row] = rows[row][column];
      }
    }
  }
  if (edit.has_scale) {
    std::memcpy(sizes, edit.scale, sizeof(sizes));
  }
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      out[row * 4 + column] = axes[column][row] * sizes[column];
    }
    out[row * 4 + 3] = edit.has_position ? edit.position[row] : level[row * 4 + 3];
  }
}

// The brush's own physical entity is suspended while it is hidden and the same entity comes back
// with all its parts (rebuilt with CBrush::Physicalize a tree kept only some of its collision);
// a brush without one is dephysicalized and rebuilt as before.
// Hides the brush and takes its collision away, or shows it with its collision again. `drawn`
// false: only its collision (a brush its layer hides, drawn or not as the layer says).
void set_brush_hidden(BrushCopy& copy, bool hide, bool drawn = true) {
  void* brush = copy.brush;
  void* world = natives.physical_world ? *natives.physical_world : nullptr;
  const bool vegetation = is_vegetation(brush);
  if (drawn) {
    slot_of<BrushHide>(brush, kBrushHide)(brush, hide);
  }
  if (hide && !copy.dephysicalized) {
    if (vegetation) {
      keep_out(brush, true);
      slot_of<BrushDephysicalize>(brush, kBrushDephysicalize)(brush, false);
    } else {
      // The brush's own physical entity is suspended and the same one comes back; a
      // brush without one is dephysicalized and rebuilt as before.
      void* physical = world ? read<void*>(brush, kBrushPhysics) : nullptr;
      if (physical) {
        slot_of<DestroyEntity>(world, kDestroyEntity)(world, physical, kSuspend, 0);
        copy.suspended = physical;
      } else {
        slot_of<BrushDephysicalize>(brush, kBrushDephysicalize)(brush, false);
      }
    }
    copy.dephysicalized = true;
  } else if (!hide && copy.dephysicalized) {
    if (vegetation) {
      keep_out(brush, false);
      slot_of<BrushPhysicalize>(brush, kBrushPhysicalize)(brush, false);
      // A small vegetation instance is the engine's on-demand physics': Physicalize builds it no
      // collision of its own (an instant one lives 5 s), so the on-demand grid is told of it again
      // the way the node's own move does (OffsetPosition by nothing), and builds it as it builds
      // the rest of that place.
      const float nothing[3] = {0.0f, 0.0f, 0.0f};
      slot_of<NodeOffsetPosition>(brush, kNodeOffsetPosition)(brush, nothing);
    } else if (copy.suspended && world && still_suspended(world, copy.suspended)) {
      slot_of<DestroyEntity>(world, kDestroyEntity)(world, copy.suspended, kRestore, 0);
    } else {
      // Instant: the physical entity is made now; left to the on-demand manager (instant false) a
      // restored brush kept no collision.
      slot_of<BrushPhysicalize>(brush, kBrushPhysicalize)(brush, true);
    }
    copy.suspended = nullptr;
    copy.dephysicalized = false;
  }
  if (drawn) {
    copy.hidden = hide;
  }
  sync_colliders(copy);
}

void apply_brush(const LevelEdit& edit, BrushCopy& copy, bool restore) {
  void* brush = copy.brush;
  draw_brush(copy, restore ? std::string() : edit.material,
             restore ? std::vector<std::string>() : edit.hidden_parts);
  const bool move = !restore && (edit.has_position || edit.has_rotation || edit.has_scale);
  if (move) {
    float matrix[12];
    brush_matrix(edit, copy.matrix, matrix);
    slot_of<BrushSetMatrix>(brush, kBrushSetMatrix)(brush, matrix);
    copy.moved = true;
  } else if (copy.moved) {
    slot_of<BrushSetMatrix>(brush, kBrushSetMatrix)(brush, copy.matrix);
    copy.moved = false;
  }
  // A copy its layer hides is drawn as the layer says, but a hide takes its collision away: an
  // invisible collision of the level's (a building's own collision brush) stayed standing where the
  // author had deleted the object. The others are hidden while the edit says so.
  if (copy.level_hidden) {
    const bool out = !restore && edit.hidden;
    if (out != copy.dephysicalized) {
      set_brush_hidden(copy, out, false);
    }
    return;
  }
  sync_colliders(copy);
  const bool hide = !restore && edit.hidden;
  const bool hidden_now = (read<std::uint64_t>(brush, kBrushFlags) & kHiddenFlag) != 0;
  if (hide != copy.hidden || hide != hidden_now) {
    set_brush_hidden(copy, hide);
  }
}

void apply_entity(Applied& applied, bool restore) {
  void* entity = applied.object;
  const LevelEdit& edit = applied.edit;
  const bool move = !restore && (edit.has_position || edit.has_rotation || edit.has_scale);
  if (move || applied.moved) {
    const float* position = move && edit.has_position ? edit.position : applied.position;
    const float* rotation = move && edit.has_rotation ? edit.rotation : applied.rotation;
    const float* scale = move && edit.has_scale ? edit.scale : applied.scale;
    slot_of<SetPos>(entity, kSetPos)(entity, position, 0, false, false);
    slot_of<SetRotation>(entity, kSetRotation)(entity, rotation, 0);
    slot_of<SetScale>(entity, kSetScale)(entity, scale, 0);
    applied.moved = move;
  }
  const bool hide = !restore && edit.hidden;
  if (hide != applied.hidden) {
    slot_of<EntityHide>(entity, kEntityHide)(entity, hide);
    applied.hidden = hide;
  }
  // Its material: the edit's, or the level's own again.
  const std::string wanted = restore ? std::string() : edit.material;
  void* material = nullptr;
  if (wanted != applied.material && material_to_draw(wanted, applied.level_material, material)) {
    slot_of<SetMaterial>(entity, kEntitySetMaterial)(entity, material);
    applied.material = material ? wanted : std::string();
  }
}

// Whether the level built a copy anew under the same render node: it shows although this game hid
// it, or stands away from where this game moved it.
bool rebuilt(const LevelEdit& edit, const BrushCopy& copy) {
  void* node = copy.brush;
  if (copy.hidden && !copy.level_hidden &&
      (read<std::uint64_t>(node, kBrushFlags) & kHiddenFlag) == 0) {
    return true;
  }
  if (!copy.moved) {
    return false;
  }
  float now[12];
  float wanted[12];
  node_matrix(node, now);
  brush_matrix(edit, copy.matrix, wanted);
  const float place[3] = {now[3], now[7], now[11]};
  const float target[3] = {wanted[3], wanted[7], wanted[11]};
  return !within_reach(place, target, kcd2s::integration::kLevelEditMatchMetres);
}

// Gives back the material references a brush copy holds (the copy is forgotten). The materials
// outlive the brush: these references are what keeps them.
void let_go(BrushCopy& copy) {
  parts::release(copy.parts_material);
  parts::release(copy.level_material);
  copy.parts_material = nullptr;
  copy.level_material = nullptr;
}

// Looks for an applied brush edit's copies again (the level streams objects out and in). A copy
// no longer found is forgotten without being touched (its node may be gone); a new copy, or one
// the level built anew, gets the edit. With no copy left the edit waits for its object again.
void recheck(Applied& applied) {
  const LevelEdit& edit = applied.edit;
  std::vector<void*> found = find_brushes(edit, edit.at, false);
  if (edit.has_position) {
    for (void* node : find_brushes(edit, edit.position, false)) {
      add_once(found, node);
    }
  }
  std::vector<BrushCopy> kept;
  std::vector<void*> kept_as_they_were;
  for (void* node : found) {
    const BrushCopy* known = nullptr;
    for (const BrushCopy& copy : applied.copies) {
      if (copy.brush == node) {
        known = &copy;
      }
    }
    if (known && !rebuilt(edit, *known)) {
      kept.push_back(*known);
      kept_as_they_were.push_back(node);
      continue;
    }
    BrushCopy copy = capture_copy(node);
    apply_brush(edit, copy, false);
    kept.push_back(copy);
    say("LEVEL edit %u: %s", edit.id, known ? "its copy was built anew" : "a new copy streamed in");
  }
  for (BrushCopy& copy : applied.copies) {
    if (copy.vegetation && std::find(found.begin(), found.end(), copy.brush) == found.end()) {
      keep_out(copy.brush, false);
    }
    // A copy gone or built anew is forgotten, with the materials it held.
    const auto same = std::find(kept_as_they_were.begin(), kept_as_they_were.end(), copy.brush);
    if (same == kept_as_they_were.end()) {
      let_go(copy);
    }
  }
  applied.copies = std::move(kept);
  if (applied.copies.empty()) {
    applied.object = nullptr;
    applied.next_try = 0;
    say("LEVEL edit %u: its object streamed out%s", edit.id, "");
    return;
  }
  applied.object = applied.copies.front().brush;
}

// Puts the edit on its object (or, restoring, takes it off); false while the object is not found.
bool apply(Applied& applied, bool restore) {
  if (!applied.object) {
    if (applied.edit.kind == LevelEditKind::Entity) {
      return false;
    }
    // The second miss names what is there instead, once (the first may be before streaming).
    const std::vector<void*> found =
        find_brushes(applied.edit, applied.edit.at, applied.misses == 1);
    if (found.empty()) {
      ++applied.misses;
      return false;
    }
    applied.object = found.front();
    for (void* brush : found) {
      applied.copies.push_back(capture_copy(brush));
    }
    // A plant's edit (Clear nature hides hundreds a stroke) is counted, not logged: a line each
    // held the game's frame up (the Architect lagged while nature was cleared).
    if (log_line && !applied.copies.front().vegetation) {
      const BrushCopy& first = applied.copies.front();
      log_line("LEVEL edit %u found %zu cop%s of %s%s%s", applied.edit.id, applied.copies.size(),
               applied.copies.size() == 1 ? "y" : "ies", applied.edit.name.c_str(),
               first.vegetation ? " (vegetation)" : "",
               first.level_hidden ? ", its layer hides it: only its collision is taken" : "");
    }
  }
  if (applied.edit.kind == LevelEditKind::Entity) {
    capture(applied);
    apply_entity(applied, restore);
  } else {
    for (BrushCopy& copy : applied.copies) {
      apply_brush(applied.edit, copy, restore);
    }
  }
  return true;
}

// A layer edit taken: switched in the next frame; a removed one gives the layer back as the
// level had it. False when the change is no layer edit.
bool take_layer(const LevelEditChange& change) {
  const std::uint32_t id = change.edit.id;
  const auto found = layer_edits.find(id);
  if (change.removed) {
    if (found == layer_edits.end()) {
      return false;
    }
    if (found->second.applied) {
      layers::restore(found->second.name);
    }
    say("LEVEL layer edit %u removed%s", id, "");
    layer_edits.erase(found);
    return true;
  }
  if (change.edit.kind != LevelEditKind::Layer) {
    return false;
  }
  if (found != layer_edits.end() && found->second.applied &&
      found->second.name != change.edit.name) {
    layers::restore(found->second.name);
  }
  LayerEdit& edit = layer_edits[id];
  edit.name = change.edit.name;
  edit.on = !change.edit.hidden;
  edit.applied = false;
  edit.next_try = 0;
  return true;
}

void apply_layers(ULONGLONG now) {
  for (auto& [id, edit] : layer_edits) {
    if (edit.applied || now < edit.next_try) {
      continue;
    }
    edit.applied = layers::set(edit.name, edit.on);
    edit.next_try = now + kRetryMilliseconds;
  }
}

void take(const LevelEditChange& change) {
  if (take_layer(change)) {
    return;
  }
  const std::uint32_t id = change.edit.id;
  const auto found = edits.find(id);
  if (change.removed) {
    if (found != edits.end()) {
      if (found->second.object && found->second.edit.kind != LevelEditKind::Entity) {
        // Only copies that still stand are touched.
        recheck(found->second);
      }
      if (found->second.object) {
        apply(found->second, true);
      }
      for (BrushCopy& copy : found->second.copies) {
        let_go(copy);
      }
      edits.erase(found);
      say("LEVEL edit %u removed%s", id, "");
    }
    return;
  }
  Applied& applied = found != edits.end() ? found->second : edits[id];
  const bool same_object = found != edits.end() && applied.edit.kind == change.edit.kind &&
                           applied.edit.guid == change.edit.guid &&
                           applied.edit.name == change.edit.name &&
                           within_reach(applied.edit.at, change.edit.at, 0.001f);
  if (found != edits.end() && !same_object) {
    // Another object under the same id: the first goes back, the second is looked for.
    if (applied.object && applied.edit.kind != LevelEditKind::Entity) {
      recheck(applied);
    }
    if (applied.object) {
      apply(applied, true);
    }
    applied = Applied{};
  }
  applied.edit = change.edit;
  applied.next_try = 0;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  installed = natives.entity_system && natives.physical_world;
  // Vegetation is a level object only while a hidden one can be kept without collision.
  bool hooked = false;
  if (natives.vegetation_matrix && natives.vegetation_physicalize) {
    hooked = MH_CreateHook(natives.vegetation_physicalize,
                           reinterpret_cast<void*>(&hooked_vegetation_physicalize),
                           reinterpret_cast<void**>(&original_vegetation_physicalize)) == MH_OK &&
             MH_EnableHook(natives.vegetation_physicalize) == MH_OK;
  }
  if (!hooked) {
    natives.vegetation_matrix = nullptr;
  }
  if (log_line) {
    log_line("LEVEL vegetation as level objects %s", hooked ? "on" : "off");
  }
  return installed;
}

bool blocks_smart_object(const float position[3]) {
  std::lock_guard<std::mutex> lock(blocked_mutex);
  for (const auto& place : blocked_places) {
    const float dx = place[0] - position[0];
    const float dy = place[1] - position[1];
    const float dz = place[2] - position[2];
    if (dx * dx + dy * dy <= kSmartObjectReach * kSmartObjectReach && std::fabs(dz) <= 2.0f) {
      return true;
    }
  }
  return false;
}

void frame(const std::vector<LevelEditChange>& changes) {
  if (!installed) {
    return;
  }
  for (const LevelEditChange& change : changes) {
    take(change);
  }
  if (!changes.empty()) {
    rebuild_blocked_places();
  }
  const ULONGLONG now = GetTickCount64();
  apply_layers(now);
  bool entities_wanted = false;
  for (auto& [id, applied] : edits) {
    if (applied.edit.kind == LevelEditKind::Entity && !applied.object && now >= applied.next_try) {
      entities_wanted = true;
    }
  }
  if (entities_wanted) {
    find_entities();
  }
  // New edits look for their objects, at most kFirstAppliesPerFrame a frame (each looks through
  // the level; hundreds at once held a frame for a second); the rest the next frames.
  int first_applies = 0;
  int plants_applied = 0;
  const auto frame_started = std::chrono::steady_clock::now();
  const auto spent = [&frame_started]() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                     frame_started)
        .count();
  };
  // The order they are looked for in: an edit never looked for first (the one the author just
  // made), then the waiting ones in turn from where the last frame stopped. In id order the first
  // ones waiting (plants whose ground streamed out) came round again before the last were reached,
  // and a new edit was never applied (a moved table of the level stayed where it
  // stood beside its copy, Trosecko with 160 edits waiting).
  std::vector<std::uint32_t> order;
  for (auto& [id, applied] : edits) {
    if (!applied.tried && now >= applied.next_try) {
      order.push_back(id);
    }
  }
  const auto resume = edits.upper_bound(resume_after);
  for (auto it = resume; it != edits.end(); ++it) {
    if (it->second.tried && now >= it->second.next_try) {
      order.push_back(it->first);
    }
  }
  for (auto it = edits.begin(); it != resume; ++it) {
    if (it->second.tried && now >= it->second.next_try) {
      order.push_back(it->first);
    }
  }
  for (const std::uint32_t id : order) {
    Applied& applied = edits[id];
    const bool had_object = applied.object != nullptr;
    if (!had_object && (first_applies >= kFirstAppliesPerFrame || spent() > kFirstApplyBudgetMs)) {
      break;
    }
    if (!had_object) {
      ++first_applies;
      if (applied.tried) {
        resume_after = id;
      }
    }
    applied.tried = true;
    if (!apply(applied, false)) {
      applied.next_try = now + kRetryMilliseconds;
      continue;
    }
    // Applied: nothing more until the server changes it.
    applied.next_try = ~ULONGLONG(0);
    applied.next_check = now + kRecheckMilliseconds;
    const bool plant = !applied.copies.empty() && applied.copies.front().vegetation;
    if (!had_object && plant) {
      ++plants_applied;
    } else if (!had_object) {
      say("LEVEL edit %u applied to its %s", id,
          applied.edit.kind == LevelEditKind::Entity ? "entity" : "brush");
    }
  }
  if (plants_applied > 0 && log_line) {
    log_line("LEVEL %d plant edits applied", plants_applied);
  }
  // Where the edits stand, every so often: how many found their objects, how many wait for them
  // (not streamed in yet), how many were never looked for.
  static ULONGLONG next_status = 0;
  if (log_line && !edits.empty() && now >= next_status) {
    next_status = now + kStatusMilliseconds;
    std::size_t found_objects = 0;
    std::size_t never_tried = 0;
    for (const auto& [id, applied] : edits) {
      found_objects += applied.object ? 1 : 0;
      never_tried += applied.tried ? 0 : 1;
    }
    log_line("LEVEL edits %zu: %zu on their objects, %zu waiting, %zu never looked for",
             edits.size(), found_objects, edits.size() - found_objects, never_tried);
  }
  // Applied brush edits look for their copies again, as many as kRecheckBudgetMs of the frame
  // takes.
  const auto rechecks_started = std::chrono::steady_clock::now();
  for (auto& [id, applied] : edits) {
    const double rechecking = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - rechecks_started)
                                  .count();
    if (rechecking > kRecheckBudgetMs) {
      break;
    }
    if (applied.edit.kind == LevelEditKind::Entity || !applied.object || now < applied.next_check) {
      continue;
    }
    recheck(applied);
    const bool plant = !applied.copies.empty() && applied.copies.front().vegetation;
    applied.next_check = now + (plant ? kPlantRecheckMilliseconds : kRecheckMilliseconds);
  }
}

void reset() {
  edits.clear();
  layer_edits.clear();
  {
    std::lock_guard<std::mutex> lock(blocked_mutex);
    blocked_places.clear();
  }
  // The next world's vegetation is its own.
  std::lock_guard<std::mutex> lock(kept_out_mutex);
  kept_out.clear();
}

std::vector<LevelBrush> brushes_near(const float center[3], float reach) {
  std::vector<LevelBrush> found;
  void* engine = natives.engine_3d ? *natives.engine_3d : nullptr;
  if (!installed || !engine) {
    return found;
  }
  const float box[6] = {center[0] - reach, center[1] - reach, center[2] - reach,
                        center[0] + reach, center[1] + reach, center[2] + reach};
  const auto objects_in_box = slot_of<ObjectsInBox>(engine, kObjectsInBox);
  const std::uint32_t count = objects_in_box(engine, box, nullptr);
  if (count == 0 || count > kMaxRenderNodes) {
    return found;
  }
  std::vector<void*> nodes(count + 16, nullptr);
  const std::uint32_t filled = objects_in_box(engine, box, nodes.data());
  for (std::uint32_t index = 0; index < filled && index < nodes.size(); ++index) {
    void* node = nodes[index];
    if (!node || !rtti_is(node, kBrushClass)) {
      continue;
    }
    LevelBrush brush;
    brush.brush = node;
    const char* mesh = slot_of<BrushName>(node, kBrushName)(node);
    brush.mesh = mesh ? mesh : "";
    std::memcpy(brush.matrix, static_cast<std::uint8_t*>(node) + kBrushMatrix,
                sizeof(brush.matrix));
    brush.layer = read<std::uint16_t>(node, kBrushLayer);
    brush.hidden = (read<std::uint64_t>(node, kBrushFlags) & kHiddenFlag) != 0;
    found.push_back(std::move(brush));
  }
  return found;
}

std::vector<LevelPlant> plants_near(const float center[3], float radius, std::size_t most) {
  std::vector<LevelPlant> found;
  void* engine = natives.engine_3d ? *natives.engine_3d : nullptr;
  if (!installed || !engine || !(radius > 0.0f)) {
    return found;
  }
  // Tall trees reach far up and down from their foot: the box spans their height.
  constexpr float kHeight = 60.0f;
  const float box[6] = {center[0] - radius, center[1] - radius, center[2] - kHeight,
                        center[0] + radius, center[1] + radius, center[2] + kHeight};
  const auto objects_in_box = slot_of<ObjectsInBox>(engine, kObjectsInBox);
  const std::uint32_t count = objects_in_box(engine, box, nullptr);
  if (count == 0 || count > kMaxRenderNodes) {
    return found;
  }
  std::vector<void*> nodes(count + 16, nullptr);
  const std::uint32_t filled = objects_in_box(engine, box, nodes.data());
  for (std::uint32_t index = 0; index < filled && index < nodes.size(); ++index) {
    void* node = nodes[index];
    if (!node || !is_vegetation(node)) {
      continue;
    }
    if ((read<std::uint64_t>(node, kBrushFlags) & kHiddenFlag) != 0) {
      continue;
    }
    alignas(16) float matrix[12] = {};
    natives.vegetation_matrix(node, matrix);
    const float dx = matrix[3] - center[0];
    const float dy = matrix[7] - center[1];
    if (dx * dx + dy * dy > radius * radius) {
      continue;
    }
    const char* mesh = slot_of<BrushName>(node, kBrushName)(node);
    if (!mesh || !*mesh) {
      continue;
    }
    LevelPlant plant;
    plant.mesh = mesh;
    plant.position[0] = matrix[3];
    plant.position[1] = matrix[7];
    plant.position[2] = matrix[11];
    found.push_back(std::move(plant));
    if (found.size() >= most) {
      break;
    }
  }
  return found;
}

void keep_vegetation_out(void* vegetation, bool out) {
  keep_out(vegetation, out);
}

bool brush_of_collider(void* collider, LevelBrush& out) {
  if (!installed || !collider) {
    return false;
  }
  float place[3];
  float bounds[6];
  if (!collider_place(collider, place, bounds)) {
    return false;
  }
  void* owner = building_at(place);
  if (!owner) {
    return false;
  }
  for (LevelBrush& brush : brushes_near(place, 1.0f)) {
    if (brush.brush == owner) {
      out = std::move(brush);
      return true;
    }
  }
  return false;
}

void take_out(void* brush) {
  if (!installed || !brush) {
    return;
  }
  BrushCopy copy;
  copy.brush = brush;
  set_brush_hidden(copy, true);
}

bool render_node_matrix(void* node, float matrix[12]) {
  if (!installed || !node) {
    return false;
  }
  return node_matrix(node, matrix);
}

bool brush_unseen(void* node) {
  if (!installed || !node || !rtti_is(node, kBrushClass)) {
    return false;
  }
  return (read<std::uint64_t>(node, kBrushFlags) & kHiddenFlag) != 0;
}

std::map<std::uint64_t, void*> entities_by_guid(const std::vector<std::uint64_t>& guids) {
  std::map<std::uint64_t, void*> found;
  void* system = installed && natives.entity_system ? natives.entity_system() : nullptr;
  void* iterator = system && !guids.empty() ? slot_of<Get>(system, kIteratorSlot)(system) : nullptr;
  if (!iterator) {
    return found;
  }
  std::map<std::uint64_t, bool> wanted;
  for (const std::uint64_t guid : guids) {
    wanted[guid] = true;
  }
  slot_of<Call>(iterator, kAddRef)(iterator);
  slot_of<Call>(iterator, kMoveFirst)(iterator);
  while (void* entity = slot_of<Get>(iterator, kNext)(iterator)) {
    const std::uint64_t key = slot_of<EntityKey>(entity, kEntityKey)(entity);
    if (wanted.count(key) != 0) {
      found[key] = entity;
    }
  }
  slot_of<Call>(iterator, kRelease)(iterator);
  return found;
}

void hide_entity(void* entity, bool hidden) {
  if (entity) {
    slot_of<EntityHide>(entity, kEntityHide)(entity, hidden);
  }
}

}  // namespace kcds::runtime::level
