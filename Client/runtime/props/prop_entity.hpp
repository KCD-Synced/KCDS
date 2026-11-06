// A held prop's entity through the game's natives: what the game's
// entity script binding does for System.SpawnEntity, LoadCharacter, SetMaterial, DestroyPhysics,
// StartAnimation, StopAnimation, SetLocalPos, SetLocalAngles and System.RemoveEntity, without a
// script (WHGame 1.5.5).
//
//   IEntitySystem   + 88 the class registry (FindClass + 24), + 96 SpawnEntity(params, init),
//                   + 128 FindEntityByName, + 152 RemoveEntity(id, now); SEntitySpawnParams from
// (class + 24, name + 56, flags + 64 / + 68, position + 80)
//   CEntity         its id at + 12, its name (a char pointer) at + 224; vtable slots 39 SetPos(pos,
//   flags, physics, force) and
//                   41 SetRotation(quat, flags) (local to the parent once attached), 64 GetWorldTM,
//                   84 SetMaterial(material), 101 GetCharacter(slot), 108 LoadGeometry(slot, file,
//                   geometry name, flags), 109 LoadCharacter(slot, file, flags); its physics
//                   component (a shared_ptr) slot 24 EnablePhysics
//   material        the 3D engine's slot 197 material manager, its slot 7
//                   LoadMaterial(name, make if not found, non-removable, flags, ...)
//   ICharacter      slot 5 its ISkeletonAnim, slot 16 its animation set (slot 6 GetAnimIDByName)
//   ISkeletonAnim   slot 9 StartAnimationById(id, params) with the 64 bytes the binding fills
//                   (transition, key time -1, speed 1, weight 1, layer, 1.0, flags 2 loop / 4
//                   hold the last key), slot 10 StopAnimationInLayer(layer, blend), slot 23 the
//                   layer's playback speed
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kcds::runtime::props::entity {

struct Natives {
  void* (*entity_system)() = nullptr;  // IEntitySystem
  void* spawn_params_init = nullptr;   // SEntitySpawnParams::SEntitySpawnParams
  void* physics_component = nullptr;   // the entity's physics component
  void* const* engine_3d = nullptr;    // I3DEngine
};

void install(const Natives& natives);

// What props the server places need beyond that.
struct PlacementNatives {
  void* set_scale = nullptr;            // CEntity::SetScale(scale vector, flags)
  void* physics_params_init = nullptr;  // SEntityPhysicalizeParams constructor
};

void install_placement(const PlacementNatives& natives);

// The entity of that name, or null.
void* find(const std::string& name);

// The entity's name (empty for none), at most max_length characters of it.
std::string name_of(void* entity, std::size_t max_length);

// Spawns a BasicEntity of that name at the world position showing the model (a .cdf character)
// with the material (none: the model's own), without physics; null when it could not.
void* spawn(const std::string& name, const float position[3], const char* model,
            const char* material);

// Spawns a BasicEntity of that name at the world position showing the mesh: a static mesh (.cgf)
// as LoadGeometry does, a character (.cdf or .chr) as LoadCharacter, with the material (none: the
// mesh's own), without physics; null when it could not (a mesh this game cannot load included).
void* spawn_mesh(const std::string& name, const float position[3], const std::string& model,
                 const std::string& material);

// Spawns the same entity as spawn_mesh, but leaves its physics to physicalize.
void* spawn_solid_mesh(const std::string& name, const float position[3], const std::string& model);

// The scale of an entity's mesh (x, y, z), as SetScale; set it before physicalize.
void set_scale(void* entity, const float scale[3]);

// How opaque the entity is drawn, 0 (unseen) to 1 (solid), as the flow node Entity:RenderParams
// sets it: the render proxy (CEntity vtable + 592, GetProxy(0)) and
// its SetOpacity (proxy vtable + 288, the alpha byte at + 168).
void set_opacity(void* entity, float opacity);

enum class Physics { None, Static, Rigid, Soft };

// Gives the entity's mesh (slot 0) a static or rigid body, as the entity binding's Physicalize;
// a rigid body weighs `mass` kilograms. None takes the body it has away (PE_NONE). Soft (type 8)
// makes it cloth of `mass` kilograms: CPhysicalProxy::PhysicalizeSoft
// builds a soft body from the model's default physics geometry (physical_props/cloth.hpp). False
// when the entity has no physics component (for None: no physics proxy).
bool physicalize(void* entity, Physics physics, float mass);

// Spawns an entity of a script class (such as StashCorpse) of that name at the world position,
// as the script SpawnEntity does with the class's own properties; null when it could not.
void* spawn_class(const std::string& name, const char* entity_class, const float position[3]);

// A value of an entity script's Properties table, set before the entity's init reads it.
struct ScriptProperty {
  const char* key = nullptr;
  const char* text = nullptr;  // a text value; null for the number
  float number = 0.0f;
  // A table of Properties the value is in (AnimDoor's Lock, Physics), null for Properties itself.
  const char* group = nullptr;
  // A true or false (the number's truth) rather than a number: a Lua script tests it as one.
  bool boolean = false;
};

// Spawns an entity of a script class (such as Ladder) of that name at the world position and
// rotation (x, y, z, w), with those Properties values in place of the class's own, as the script
// SpawnEntity does with `properties`: the entity is spawned without its init (IEntitySystem slot
// 12, SpawnEntity(params, false)), the values go into its script's Properties table (entity slot
// 74 GetProxy(2) and the proxy's slot 18, IScriptTable slot 8 GetValueAny and slot 7
// SetValueAny), then it is initialised (slot 13, InitEntity). Null when it could not.
// `guid`, when not 0, is the entity's EntityGuid (SEntitySpawnParams + 8; entity slot 2 gives it
// back): the same on every machine for an entity the server names (a door).
void* spawn_class_with(const std::string& name, const char* entity_class, const float position[3],
                       const float rotation[4], const std::vector<ScriptProperty>& properties,
                       std::uint64_t guid = 0);

// Removes the entity of that name, when there is one.
void remove(const std::string& name);

// The entity's world position (its world matrix's translation).
void world_position(void* entity, float position[3]);

// The entity's world position and turn (x, y, z, w), its scale left out.
void world_pose(void* entity, float position[3], float rotation[4]);

// The entity's world scale along its own x axis (its world matrix's first column).
float world_scale(void* entity);

// The file of the static mesh in the entity's slot 0 ("objects/....cgf" as the game loaded it),
// empty when it shows none: CEntity vtable slot 103 GetStatObj(slot) and the mesh's slot 41
// GetFilePath.
std::string mesh_file(void* entity, std::size_t max_length);

// The entity's slot 0 shows that static mesh (.cgf) in place of the one it showed, as
// LoadGeometry does; false when the game could not load it.
bool show_mesh(void* entity, const std::string& model);

// Draws the entity with that material in place of its mesh's own (the material manager's
// LoadMaterial, the entity's slot 84 SetMaterial); its physics stays as it was built.
void draw_with_material(void* entity, const std::string& material);

// The entity drawn anew where it stands, so a material set on it after it was drawn shows (live
// a texture on a studio mesh showed only once the mesh was moved). SetWorldTM to the
// place it has changes nothing (SetPosRotScale, compares each part and only then
// tells the entity it moved), so it goes a tenth of a millimetre up and back.
void redraw(void* entity);

// The name of the material the entity is drawn with in place of its mesh's own, empty for none:
// CEntity vtable slot 85 GetMaterial and the material's slot 8 GetName.
std::string material_name(void* entity, std::size_t max_length);

// Where a joint of the entity's character (slot 0) is in the world, by the joint's name, as the
// entity binding's GetBonePos finds it: the character's default skeleton (character
// slot 11) and its GetJointIDByName (slot 7), the skeleton pose (character slot 7) and its
// GetAbsJointByID (slot 27, a QuatT in the character's space), then the entity's world matrix.
// False without a character or such a joint.
bool joint_pose(void* entity, const char* joint, float position[3], float rotation[4]);

// Whether the entity's character has a joint of this name (its default skeleton's
// GetJointIDByName); false without a character.
bool has_joint(void* entity, const char* joint);

// The id of the entity's character's joint of this name (GetJointIDByName); -1 without one.
int joint_id(void* entity, const char* joint);

// The joint of the entity's character a script names: the name itself when the body
// has it, else CryEngine's biped name the way the game's skeleton names it ("Bip01 Neck" is
// "Neck", "Bip01 R Hand" "RightHand"), else "".
std::string body_joint_name(void* entity, const std::string& wanted);

// The names of the entity's character's joints in their order (IDefaultSkeleton slot 6 for each
// id; the count before CDefaultSkeleton's joints, + 8); none without a character.
std::vector<std::string> joint_names(void* entity);

// How many joints the entity's character has (IDefaultSkeleton slot 1, GetJointCount);
// 0 without a character.
int joint_count(void* entity);

// A joint's parent (IDefaultSkeleton slot 2: the joint record's parent at + 22, of
// 48 bytes each); -1 for the root, an unknown id or no character.
int joint_parent(void* entity, int id);

// Where a joint is in the world by its id, as joint_pose by its name; false without it.
bool joint_pose_by_id(void* entity, int id, float position[3], float rotation[4]);

// Where the first `count` joints are in the world (x, y, z each in `positions`), as
// joint_pose_by_id, with one look-up of the character, its pose and the body's world pose for
// all of them; `known` says which were found. False without a character.
bool joint_places(void* entity, int count, float* positions, std::vector<bool>& known);

// The entity's character (ICharacterInstance of slot 0, entity slot 101); null without one.
void* character(void* entity);

// A joint of the entity's character in the skeleton's default pose (IDefaultSkeleton slot `which`:
// 8 or 9, the default absolute or relative QuatT), in the character's space; false without one.
bool default_joint(void* entity, const char* joint, int which, float position[3],
                   float rotation[4]);

// A helper of the entity's model in slot 0 (CEntity slot 103 GetStatObj, IStatObj slot 47
// GetHelperTM, as the entity binding's GetHelperPos reads it), in the model's space;
// false without a model.
bool helper_pose(void* entity, const char* helper, float position[3], float rotation[4]);

// The entity of that id (IEntitySystem slot 14, GetEntity), or null.
void* by_id(std::uint32_t id);

// The entity's id.
std::uint32_t id_of(void* entity);

// Calls `visit` with every entity of the class named `class_name` (the entity system's iterator,
// as the game's System.GetEntitiesByClass walks it: the class registry and FindClass, the
// iterator's AddRef, MoveFirst, Next and Release, each entity's class at slot 3).
void each_of_class(const char* class_name, const std::function<void(void* entity)>& visit);

// Calls `visit` with every entity the entity system holds, through the same iterator.
void each_entity(const std::function<void(void* entity)>& visit);

// Sets a boolean field of the entity's script table (IScriptTable SetValueAny, as the script
// binding writes a value); false when the entity has no script.
bool set_script_flag(void* entity, const char* key, bool value);

// A number field of the entity's script table (IScriptTable GetValueAny); false when the entity
// has no script or the field is not a number.
bool script_number(void* entity, const char* key, float& value);

// Sets a number field of the entity's script table; false when the entity has no script.
bool set_script_number(void* entity, const char* key, float value);

// The ISkeletonAnim of the entity's character (slot 0), null without one.
void* skeleton_anim_of(void* entity);

// Shows a character model (.cdf) in a slot of the entity, as LoadCharacter; false when it could
// not.
bool load_character(void* entity, int character_slot, const char* model);

// Turns an entity in the world: a rotation quaternion (x, y, z, w), as SetWorldRotation.
void rotate(void* entity, const float rotation[4]);

// Places an entity where it hangs from its parent (or in the world before it has one): position
// in metres and angles in radians about x, y and z, as SetLocalPos and SetLocalAngles.
void place(void* entity, const float position[3], const float angles[3]);

// The same with a rotation quaternion (x, y, z, w) for the angles.
void place_rotated(void* entity, const float position[3], const float rotation[4]);

// Places the entity at a world position and turn, keeping its scale, through its world matrix
// (CEntity slot 30, SetWorldTM): for an entity that place_rotated puts elsewhere (the
// alchemy table's recipe book on another client went far off the room).
void place_in_world(void* entity, const float position[3], const float rotation[4]);

struct Clip {
  const char* name = nullptr;
  float blend = 0.15f;  // seconds into it from what played before
  float speed = 1.0f;   // 0 holds it on its first frame
  bool loop = false;    // otherwise it holds its last frame
};

// Plays a clip on layer 0 of the entity's character; false when it has no such clip.
bool play(void* entity, const Clip& clip);

// Puts layer 0 of the entity's character at a point of its clip (0 its first frame, 1 its last),
// as the entity binding's SetAnimationTime; with the clip held (speed 0) the caller drives it,
// forwards or backwards.
void set_time(void* entity, float time);

// Stops what layer 0 of the entity's character plays, at once.
void stop(void* entity);

// The box the entity takes in the world (CEntity slot 34, GetWorldBounds); false when it bounds
// nothing yet (its mesh not loaded).
bool world_bounds(void* entity, float min[3], float max[3]);

// The box around the entity in its own frame (CEntity slot 35, GetLocalBounds,
// which GetWorldBounds calls); false when it has none.
bool local_bounds(void* entity, float min[3], float max[3]);

// Hides the entity or shows it again, as the entity binding's Hide (CEntity slot 63).
void hide(void* entity, bool hidden);

// Whether the entity is hidden (CEntity slot 64, IsHidden).
bool is_hidden(void* entity);

}  // namespace kcds::runtime::props::entity
