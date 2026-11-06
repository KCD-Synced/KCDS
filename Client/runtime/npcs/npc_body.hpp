// The body of a server's NPC in this game, WHGame 1.5.5.
//
// A body is the game's own NPC entity with the soul the server named, made the way the game's
// script binding System.SpawnEntity makes one with `properties` (CScriptBind_System::SpawnEntity):
// the entity is spawned without its init (IEntitySystem slot 12, SpawnEntity(params,
// false)), the soul GUID goes into the entity script's Properties table as `sharedSoulGuid`, and
// then the entity is initialised (slot 13, InitEntity(entity, params)). The game then gives the
// body that soul: its name, its clothes and its brain.
//
//   SEntitySpawnParams initialises it; class + 24, name + 56, flags + 64 (0x1000000) and
//                       + 68 (0x400000) as the binding sets them, position + 80, rotation + 92
//   entity script table entity slot 74 GetProxy(2, the script proxy), its slot 18
//   IScriptTable        slot 7 SetValueAny(key, value, chain), slot 8 GetValueAny(key, value,
//                       chain), slot 4 Release; a ScriptAnyValue is its type (5 a string, 6 a
//                       table) and the value at + 8
//
// The class decides how the body acts: "NPC" (and "NPC_Female") runs the soul's brain, the body
// the client that simulates the NPC shows; "NPC_NAI" has no AI, the body every other client shows
// and moves as the simulator reports (both took the soul and its clothes; the
// game dresses each body anew, so the clothes differ between bodies of one soul).
//
// The bodies are made inside npcs::OwnSpawn, so the refusal of the game's own NPCs lets them
// through. Engine thread only.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::npcs {

struct BodyNatives {
  void* (*entity_system)() = nullptr;  // IEntitySystem
  void* spawn_params_init = nullptr;   // SEntitySpawnParams::SEntitySpawnParams
};

void install_bodies(const BodyNatives& natives);

// Makes a body of `entity_class` named `name` at the position (x, y, z) and rotation (x, y, z, w)
// with the soul `soul` (a GUID text; "" for the class's generic soul). Null when the class is
// unknown or the game refused the entity. `female`: the body (of the NPC class without AI) is a
// woman's: NPC_Female's model, clothing, animations and controller are set before its init.
void* spawn_body(const std::string& name, const char* entity_class, const std::string& soul,
                 const float position[3], const float rotation[4], bool female = false);

// Sets a boolean in the entity's own script table (IScriptTable::SetValueAny), such as a horse
// script's mountIsLegal that its interaction reads. False without the entity's script table.
bool set_script_flag(void* entity, const char* key, bool on);

// A player's body that is a woman's: the female skeleton as its model (fileModel and
// clientFileModel, PlayerFemale.lua's) and the female clothing config, set on an entity spawned
// without its init; the player's own animation database stays.
bool set_female_model(void* entity);

// A player's body's model (fileModel and clientFileModel) in its script's properties, so the game's
// own resets load it (appearance/player_skeleton.hpp).
bool set_player_model(void* entity, const char* model);

// Initialises an entity spawned without its init (IEntitySystem slot 13). False when refused.
bool init_entity(void* system, void* entity, void* params);

// Plays a clip of the game's own animations on the body (by its name, the file's without the
// extension, or by a path that ends in it), looped or holding its last key, on the layer a
// player's script clip uses (12), as player_animation does: the body's character (entity slot
// 101), its animation set (character slot 16, GetAnimIDByName slot 6) and its skeleton (character
// slot 5, StartAnimationById slot 9). False when the body has no such clip.
bool play_clip(void* entity, const std::string& clip, bool loop);

// Blends out what play_clip started (StopAnimationInLayer, ISkeletonAnim slot 10).
void stop_clip(void* entity);

// Plays what an animation field says ("<sequence>|<loop 0/1>|<clip>", npc.hpp NpcAnimation), or
// stops the clip for "". False when the field cannot be read or the body has no such clip.
bool apply_animation_field(void* entity, const std::string& field);

// A fragment request in an animation field: "<sequence>|<loop
// 0/1>||<fragment>|<tags>", an empty clip and then the fragment and its tags separated by '+'.
struct FragmentField {
  std::uint32_t sequence = 0;
  bool loop = false;
  std::string fragment;
  std::string tags;
};

// False for a clip, a stop or a field that cannot be read.
bool fragment_field_of(const std::string& field, FragmentField& request);

// The newest clip on the layer play_clip uses (ISkeletonAnim slots 15 and 18: its count and one
// of them, the clip id at + 16; slot 28 its normalized time, 0 to 1); false while it holds none.
bool layer_clip(void* entity, std::int32_t& clip, float& time);

// Builds the actor's body from an appearance field ("<body>|<head>|<hair>|<beard>", npc.hpp
// NpcAppearance; an empty name the soul's own), as a player's own look is put on
// (appearance/components.hpp). False when the field cannot be read or the soul refused it.
bool apply_appearance_field(void* actor, const std::string& field);

}  // namespace kcds::runtime::npcs
