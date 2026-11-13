// The particle effects a station worker's own animation plays on a hand, seen by every player:
// the stream of water, wine, oil or spirit poured from a flask into the cauldron and the herbs
// poured from a bowl at an alchemy table.
//
// The game plays them as mannequin procedural clips of type ParticleEffect (male_alchemy.adb:
// WH_Particels.other.water_pouring and the like on the joint LeftHandSlot or RightHandSlot).
// CProceduralClipParticleEffect (120 bytes: its scope at + 24, its params copied to + 40, the
// effect's name at + 48, the joint's CRC32 of its lower-case name at + 56, the position offset
// at + 64 and the rotation offset, in radians about x, y and z, at + 76) starts the effect when
// its clip enters (vtable slot 12) and stops it when it exits (slot 4): on a bone attachment cloned
// on that joint it binds a CEffectAttachment (the effect, the offset, and as its direction the
// rotation's y axis). A replica plays its owner's clips but no mannequin fragments, so it never
// played these.
//
// The owner's client notes the clips its player's body enters and leaves (their scope's entity is
// the player's), and the alchemy field carries each running one on a hand slot as an entry "#fx<n>"
// (kcd2s/integration/station_action.hpp): its effect's name as the model, the offset and rotation
// as the place, the hand as the place's hand. Every other client binds the same effect on the same
// joint of the replica's body, as the game's Lua SetAttachmentEffect does (a bone attachment of its
// own, CEffectAttachment by the effect's name: 56 bytes of the game's allocator; IAttachment
// AddBinding, slot 26, HideAttachment, slot 17, ClearBinding, slot 30), while the entry stays.
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <string_view>

#include "kcd2s/integration/station_action.hpp"

namespace kcds::runtime::crafting::hand_effects {

using Logger = void (*)(const char* format, ...);

struct Natives {
  // CProceduralClipParticleEffect's OnEnter (this, blend time, duration, params) and OnExit
  // (this, blend time), hooked.
  void* clip_enter = nullptr;
  void* clip_exit = nullptr;
  // The game's allocator (void*(size)) and CEffectAttachment's constructor (memory, the effect's
  // name, the offset, the direction, the scale).
  void* allocate = nullptr;
  void* effect_attachment = nullptr;
  Logger log = nullptr;
};

// Hooks the clip's enter and exit; false when they are missing.
bool install(const Natives& natives);

// Whether an entry key is one of the hand effects'.
bool is_effect_key(std::string_view key);

// The owner's side: the effects the body of the entity `local_entity` plays on a hand now, added
// to the action as entries.
void add_own(kcd2s::integration::StationAction& action, std::uint32_t local_entity);

// Every other client's side: the replica's body plays the action's hand effects.
void keep(std::uint32_t holder, void* body, const kcd2s::integration::StationAction& action);

// The replica left the table or is gone: its effects stop.
void forget(std::uint32_t holder);

// The world was reset: the attachments went with the bodies.
void reset();

}  // namespace kcds::runtime::crafting::hand_effects
