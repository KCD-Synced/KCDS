// Cloth (Asset/Animation Studio): a mesh entity made a soft body by the game's own
// physics (CSoftEntity), the way the game physicalizes a model as soft.
//
// WHGame:
//   - gEnv->pPhysicalWorld slot 4 (+ 32) gives the geometry manager (CGeomManager, vtable);
// its CreateMesh (slot 3) takes strided vertices and 16-bit
//     indices ({ pointer, stride } each), materials, foreign ids, the triangle count, flags, the
//     primitive tolerance (0: a mesh) and SMeshBVParams { min, max triangles per node, favour
//     AABB }; RegisterGeometry (slot 7) makes it a phys_geometry;
//   - the model (IStatObj, CEntity slot 103) takes it as its default physics geometry with
//     SetPhysGeom (CStatObj slot 27, type 0x1000, the one GetPhysGeom gives);
//   - the entity's Physicalize with type 8 (CPhysicalProxy::PhysicalizeSoft)
//     builds the soft body from that geometry (props/prop_entity.hpp Physics::Soft).
//
// Engine thread.
#pragma once

#include <cstdint>
#include <vector>

namespace kcds::runtime::physical_props::cloth {

// The global holding gEnv->pPhysicalWorld (physics/physics_install.cpp).
void install(void* const* physical_world);

// Gives the entity's model (slot 0) a default physics geometry made of `vertices` (x y z each, the
// model's own space) and `indices` (three per triangle), as the game's compiler gives its own
// models one (a mesh of ours has none, so a body on it weighed 0 kg). The entity's
// Physicalize builds its body from it afterwards. Returns the mesh geometry (IGeometry), null
// (logged) when a native or the model is missing or the game made none.
void* give_collision(void* entity, const std::vector<float>& vertices,
                     const std::vector<std::uint16_t>& indices);

// Makes the entity's mesh cloth of `mass` kilograms: `vertices` (x y z each, the mesh's own
// space, as its model has them) and `indices` (three per triangle) as its physics mesh, with the
// vertices `pinned` (their numbers) held where they are in the world. False (logged) when a
// native or the model is missing or the game made no geometry.
//
// The vertices must be the model's own, in its order (studio/cgf_mesh.hpp): the model's
// SyncToRenderMesh moves drawn vertex n with physics vertex n.
//
// A cloth only simulates once it was given its attached vertices: CSoftEntity::Step (slot 30)
// returns at once while its connected-vertex count (+376) is 0, which its
// AddGeometry leaves 0 for a cloth (no lattice) and pe_action_attach_points (type
// 11, Action slot 7: entity +8, -10 the world; part +16; vertex numbers +24; points +32, -1 where
// they are; count +40) sets: the attached vertices (+380) and every vertex an edge path joins to
// them (+376). With no vertex pinned the cloth stays still.
bool make(void* entity, const std::vector<float>& vertices,
          const std::vector<std::uint16_t>& indices, float mass, const std::vector<int>& pinned);

// Every frame while a cloth is shown: keeps its physics on and sends its pinned vertices until
// the body holds them (the game drops the action while the body's physics is off, live).
void tick();

// Forgets the cloth made on `entity` (any, when null) before that entity goes.
void forget(void* entity);

// Keeps the cloth's physics on: the game turns an entity's physics off a moment after it is made
// (its simulation class -1); EnablePhysics (the physics proxy's slot 24) turns it on again.
void keep_on(void* entity);

}  // namespace kcds::runtime::physical_props::cloth
