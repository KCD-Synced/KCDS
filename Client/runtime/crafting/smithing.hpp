// Smithing at a smithy's forge and anvil, seen by every player,
// WHGame 1.5.5.
//
// The owner's game runs the minigame: C_Blacksmithing (RTTI ".?AVC_Blacksmithing@playermodule@
// wh@@", vtable slot 19 its frame update, slot 5 its stop) keeps the user's
// actor at + 24, the station (a Smithery entity) at + 40 and whether it runs at + 44. Its context
// ((+ 104) slot 12) holds the attachments the minigame moves: the hammer (+ 736), the tongs
// (+ 744), the bellows' lever (+ 752), made from the station's links of those names, and the
// workpiece (+ 760), a GeomEntity named after the recipe's workpiece. An attachment keeps its
// slot at + 8 (0 the right hand, 1 the left), the entity at + 16 and whether it hangs from the
// hand at + 20. The body's animation already travels as every animation does; the owner's client
// reports each attached thing's place relative to the hand that holds it (measured once while the
// hand is steady) or relative to the station, in the NetworkPlayer field "stationAction"
// (kcd2s/integration/station_action.hpp).
//
// Every other client shows that on its replica: a copy of the hammer and the tongs (the item
// classes' own models) and of the workpiece (the workpiece's model) hang from the replica's hand
// joints at the reported offsets or lie where the station's frame puts them, the station's own
// hammer and tongs are hidden meanwhile. When the field empties everything goes back as the level
// had it.
//
// The bellows (the forge bag, forge_bag2 at Tachov, the bellows attachment's entity) is a
// character of its own that the smith's animation drives as its slave object: the forge's
// fragments (BlacksmithingForgeIdle and the rest, scope context "forgeBag") play the bag's own
// blend space on it, and the forge part's update eases the pull towards 1 while the
// player pumps and 0 otherwise, setting it as motion parameter 11 (BlendWeight2) on the
// smith's skeleton and on the bag's (entity slot 101 its character, character slot 5
// its ISkeletonAnim, slot 25 SetDesiredMotionParam); every minigame motion parameter
// goes to both. The owner's client reports the bag's own clip (its layer 0 FIFO); every other
// client plays that clip on its bag and gives it the replica's relayed motion parameters 10 and 11
// every frame, so the lever moves with the replica's hands. On another client the game does not run
// the bag's clips (nothing processes the level entity's character there; a blend space stood at
// rest or vanished, a clip's time stood still, live), but a clip's start puts the bag in that
// clip's pose: the bag plays the blend space's example nearest to the replica's pull (and the
// sword's reach), and while it pumps its loop starts again every frame at the body's blend-space
// time (ISkeletonAnim slot 28), so the lever follows the replica's hands.
//
// The station's effects are the level's own entities linked to it, which every client has: the
// forge's light (forgeLight) and its flare and heat (forgeStartParticles, forgeHeatingParticles),
// the anvil's sparks (anvilParticles), the quench's steam (hardeningParticles) and the smoke of an
// overheated workpiece (qualityLoss_...). The minigame's forge part (+ 168) keeps their ids (the
// flare + 60, the heat + 64, the light + 72) and the light's own colour (+ 76); the light's colour
// now is its render node's (entity slot 90 by slot, node slot 75) light at + 32. The owner's
// client reports which burn, the light against its own colour, and a count of every burst the
// game sets off (the particle switch's hook); every other client turns its own on and off the same
// way.
//
// The workpiece glows as the minigame makes it (C_BlacksmithingWorkpieceAttachment's heat zones,
// a vector at + 40 / + 48, each zone's index at + 4 and its temperature at + 8; gives the
// zone's part "hot_<index + 1>" its own material and sets "emissive_color" and
// "emissive_intensity" from Libs/Tables/minigame/BlacksmithTemperatures.xml): the owner's client
// reports each zone's heat, every other client sets the same on its copy's parts (IMaterial slot 47
// SetGetMaterialParamVec3, slot 46 SetGetMaterialParamFloat).
//
// Engine thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kcds::runtime::crafting::smithing {

using Logger = void (*)(const char* format, ...);

struct Natives {
  const std::uint8_t* image = nullptr;  // WHGame.dll as loaded
  std::size_t image_size = 0;
  void* attach_child = nullptr;  // CEntity::AttachChild
  // A ParticleEffect entity's particle component turned on (restarted when on) or off
  // (its entity at + 40, the emitter's slot at + 104, whether it burns at + 108).
  void* particle_enable = nullptr;
  // The particle component of an entity (proxy 18, its interface of that kind).
  void* particle_component = nullptr;
  // A particle component activated: its effect loaded into a slot when it has none,
  // then switched on (switching alone only marks an effect without an emitter as burning).
  void* particle_activate = nullptr;
  // A particle component restarted as a stroke restarts the anvil's sparks (stopped
  // when it has an emitter, then activated), so the new burst takes the stroke's count.
  void* particle_restart = nullptr;
  // The workpiece hung on the tongs as the game hangs it: the game's allocator
  // (void*(size)) and CEntityAttachment's constructor (memory, the bound
  // entity, its holder, flags).
  void* allocate = nullptr;
  void* entity_attachment = nullptr;
  // What a hand holds, held as the game holds a smith's tools and an iron without tongs
  // (C_BlacksmithingToolAttachment): the actor of an entity (IActorSystem::GetActor),
  // and its hand slots' holder (IActor slot 194) taking an entity into a slot (holder,
  // entity, slot, attachment name, joint or null, 1) or letting it go (holder, slot,
  // attachment name, 0, 1).
  void* (*actor_of)(std::uint32_t entity) = nullptr;
  void* hand_take = nullptr;
  void* hand_release = nullptr;
  // A material of its own for a part of an entity's character (the entity's material
  // cloned once, given to the attachment of that name; out: its sub-material of that name).
  void* part_material = nullptr;
  // The minigame's glow of one heat zone: the zone's part "hot_<index+1>"
  // gets a material of its own and its emissive colour and intensity fade to the zone's heat over
  // a second through the material animations, as the owner's workpiece glows. Setting the
  // material's parameters directly made the renderer rebuild the material and the piece went
  // black for a few frames at every change (video).
  void* zone_glow = nullptr;
  // Whether a user may smith at a station ((station, user): near enough and free to
  // play a minigame); the start of the minigame asks it too.
  void* can_use = nullptr;
  // The recipe book's close action (its capture holding the minigame at + 8).
  void* close_recipe_book = nullptr;
  // A clip started on a skeleton's layer 0 with the given flags, blend and start time (its
  // normalized key time, with CA_START_AT_KEYTIME; ISkeletonAnim::StartAnimationById past the clip
  // hooks), and that layer's clips stopped.
  bool (*start_clip)(void* skeleton, int clip, std::uint32_t flags, float blend,
                     float key_time) = nullptr;
  void (*stop_clips)(void* skeleton) = nullptr;
  // That layer's clips removed at once (ISkeletonAnim::ClearFIFOLayer past the clip hooks).
  void (*clear_clips)(void* skeleton) = nullptr;
  Logger log = nullptr;
};

// A replica's motion parameters as its owner's game set them (bit n of the mask: parameter n).
struct BodyMotion {
  std::uint32_t mask = 0;
  float values[17] = {};
};

// Hooks the minigame's frame update (found by its RTTI name); false when it is missing.
bool install(const Natives& natives);

// Every frame on the owner's side: the field "stationAction" for the local player, "" while it
// smiths nowhere.
std::string own_field(void* local_actor, void* local_entity, double now);

// Every frame for a replica: its body (null when it has none) shows the field's action.
void keep(std::uint32_t holder, void* body, const std::string& field, double now,
          const BodyMotion& motion);

// The replica is gone: what it moved goes back.
void forget(std::uint32_t holder);

// The world was reset: the copies went with it and nothing is moved any more.
void reset();

// Whether a replica works at the station standing at that position: the game's check whether
// the local player may smith there (can_use) says no meanwhile, so one player smiths at a station.
bool station_taken(const float position[3]);

// The smithing recipe the local player chose at a station since the last call (its id, and where
// the station stands), or "".
std::string take_chosen_recipe(float station[3]);

// Whether the game broke the local player's workpiece since the last call (the minigame's state
// BrokenWorkpiece), and where the station stands.
bool take_broken_workpiece(float station[3]);

// Whether the local player's minigame runs at a station now.
bool own_running();

// The server's Crafting.cancel: the local player's minigame at a station exits as the player's
// own exit would; false when the local player is at none.
bool close_own();

}  // namespace kcds::runtime::crafting::smithing
