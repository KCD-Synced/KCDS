// The server's level edits in the game (LevelEdit), WHGame 1.5.5.
//
// An edit hides a level object (`hidden`), relocates it, or does both, and
// is undone when the server removes it. The server sends every edit of the player's world when
// the world gets ready, so a late joiner gets them all; nothing here is sent back.
//
//   entity  A level entity is found by its EntityGuid: the entity system's iterator (slot 22:
//           AddRef 1, Release 2, Next 4, MoveFirst 6) and each entity's key (slot 2). It is hidden
//           with CEntity::Hide (vtable slot 63, as the script's Entity.Hide), which
//           also hides its children, and moved with SetPos (slot 39), SetRotation (41) and
//           SetScale (43). Where the level put it is read once, before the first
//           change: GetPos (40, + 48), GetRotation (42, + 60) and GetScale (44, + 76).
//   brush   A brush (a CBrush render node, no entity) is found through the physics it has:
//           gEnv->pPhysicalWorld's GetEntitiesInBox (vtable slot 34) around the
//           place, then each static physical entity's foreign data (slots 10 and 11: type 1,
//           PHYS_FOREIGN_ID_STATIC, is the brush itself, as CBrush::Physicalize
//           registers it), checked by its RTTI name ".?AVCBrush@@", its mesh (GetName, slot 9,
//           the stat object's path) and its matrix's translation (+ 80, 3x4 rows). It is hidden
//           with IRenderNode::Hide (slot 68, ERF_HIDDEN, 0x100 of the flags at + 40)
//           and its collision taken away with Dephysicalize (slot 48); it gets them
//           back with Hide(false) and Physicalize (slot 33). It is moved with
//           SetMatrix (slot 16). A brush without physics (or whose physics is not
//           found) is looked for in the 3D engine's object tree instead: C3DEngine::
//           GetObjectsInBox(box, nodes) (vtable slot 243; the count without
//           `nodes`), each render node checked as above. The level may hold the same brush
//           several times at one place, one per layer (CBrush + 0x92 its layer id, slot 44); the
//           edit takes every copy, and leaves a copy its layer hides as the layer has it.
//           A vegetation instance (a CVegetation render node: a tree, bush or plant the level
//           paints on) is a brush to an edit too, found the same ways and hidden,
//           dephysicalized and physicalized through the same IRenderNode slots. It keeps no
//           matrix: CVegetation::CalcMatrix builds it from its place (+ 80), its
//           three turns (bytes + 133, + 134, + 124, of 255 a full turn) and its size (byte + 125,
//           in 64ths). Its SetMatrix (slot 16) takes the place only, so an edit moves a
//           vegetation instance but does not turn or size it. Small vegetation has the engine's
//           on-demand physics: a collision built when a player comes near, with a 5 s lifetime
//           (Physicalize). A hidden instance is kept out of it (Physicalize is hooked to
//           refuse it); a restored one is put back in its grid by OffsetPosition (slot 20)
// by nothing, as its own move does.
//
// An object not found yet (not streamed in, or no physics yet) is looked for again every
// kRetryMilliseconds. The level streams objects out and in again (a tree the player walked away
// from comes back as a new render node), so an applied brush edit looks for its copies again every
// kRecheckMilliseconds: a copy gone is forgotten untouched, a new or rebuilt one gets the edit, and
// a restore touches only copies that still stand. Engine thread.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "kcd2s/integration/level_edits.hpp"

namespace kcds::runtime::level {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*entity_system)() = nullptr;     // IEntitySystem
  void* const* physical_world = nullptr;  // the global holding gEnv->pPhysicalWorld
  void* const* engine_3d = nullptr;       // the global holding the I3DEngine
  // CVegetation::CalcMatrix(vegetation, out): its world matrix, 3x4 by rows, into a 16-byte
  // aligned buffer. Without it vegetation is no level object.
  void(__fastcall* vegetation_matrix)(void* vegetation, float* matrix) = nullptr;
  // CVegetation::Physicalize, hooked so the engine's on-demand physics does not give a
  // vegetation instance an edit hides its collision back. Without it vegetation is no level
  // object either.
  void* vegetation_physicalize = nullptr;
};

// False when a native is missing; edits are then not applied in this game.
bool install(const Natives& natives, Logger logger);

// Every frame in a ready world with what the server sent since the last frame, oldest first.
void frame(const std::vector<kcd2s::integration::LevelEditChange>& changes);

// Whether a smart object at `position` (a seat, a bench, a place to work) belongs to an object an
// edit hid or moved away (issue 016: NPCs and dogs sat on benches the world's edits had taken
// away): it lies within kSmartObjectReach of where the level had a hidden or moved object that
// is not vegetation. Any thread.
bool blocks_smart_object(const float position[3]);

// The world is left: the objects are the next world's, so nothing is undone, only forgotten.
void reset();

// ---- For this game's own modules (the gates' series, gates/series.hpp) -------------------------

// A brush of the level, as the 3D engine's object tree has it.
struct LevelBrush {
  void* brush = nullptr;
  std::string mesh;
  float matrix[12] = {};  // its world matrix, 3x4 by rows
  std::uint16_t layer = 0;
  bool hidden = false;  // its layer (or anything else) hides it
};

// The level's brushes whose bounds reach into the box `reach` metres around `center`, of any
// layer, shown or hidden.
std::vector<LevelBrush> brushes_near(const float center[3], float reach);

// Keeps a vegetation instance out of the engine's on-demand physics (it is never physicalized
// again while out), as an edit does for a hidden one: World Architect's removed plants, so a
// removed or moved tree leaves no invisible wall where it stood. Any thread.
void keep_vegetation_out(void* vegetation, bool out);

// A vegetation instance the level painted (a tree, bush or plant): its mesh and its place.
struct LevelPlant {
  std::string mesh;
  float position[3] = {};
};

// The shown vegetation instances within `radius` metres of `center` on the ground plane, at most
// `most` (World Architect's nature brush).
std::vector<LevelPlant> plants_near(const float center[3], float radius, std::size_t most);

// The brush a level collider without foreign data stands for (a building's own collider, at
// exactly its brushes' place: the one with physics geometry, the largest); false for none.
bool brush_of_collider(void* collider, LevelBrush& out);

// Takes a shown brush out of this world (hidden, without collision); the next world has it back.
void take_out(void* brush);

// The world matrix (3x4 by rows) of a level object a ray met: a brush's own, or the one a
// vegetation instance's CalcMatrix builds. False for any other render node, or vegetation while
// CalcMatrix is not found.
bool render_node_matrix(void* node, float matrix[12]);

// Whether a brush's layer hides it: drawn nowhere, its collision standing (an invisible collision
// brush of the level's). False for anything that is not a brush.
bool brush_unseen(void* node);

// The level entities of those EntityGuids that exist in this world, in one walk of the entity
// system: guid -> CEntity.
std::map<std::uint64_t, void*> entities_by_guid(const std::vector<std::uint64_t>& guids);

// Hides a level entity or shows it again (CEntity::Hide).
void hide_entity(void* entity, bool hidden);

}  // namespace kcds::runtime::level
