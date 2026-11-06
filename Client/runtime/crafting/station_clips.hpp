// The clips a station's own animated things play (the forge bag's lever, an alchemy table's
// bellows): read on the owner's game, named so they travel, and started on another client's copy
// (smithing.hpp, alchemy.hpp), WHGame 1.5.5.
//
// An entity's ISkeletonAnim is its character's (entity slot 101, character slot 5). Its layer 0
// keeps the clips it plays (slot 15 their count, slot 18 one of them: a CAnimation with the clip
// id at + 16, the start flags at + 76 and the parametric block at + 8). The character's animation
// set (character slot 16) gives a clip's id by name (slot 6) and a name by id (slot 7):
// a blend space's id differs between clients, its name does not.
//
// Engine thread only.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::crafting::clips {

// CryCharAnimationParams flags: loop, start at the key time.
constexpr std::uint32_t kLoopFlag = 0x2;
constexpr std::uint32_t kStartAtKeyTime = 0x10;

struct Natives {
  // A clip started on a skeleton's layer 0 with the given flags, blend and start time (its
  // normalized key time, with kStartAtKeyTime; ISkeletonAnim::StartAnimationById past the clip
  // hooks), and that layer's clips stopped.
  bool (*start_clip)(void* skeleton, int clip, std::uint32_t flags, float blend,
                     float key_time) = nullptr;
  void (*stop_clips)(void* skeleton) = nullptr;
  // That layer's clips removed at once (ISkeletonAnim::ClearFIFOLayer past the clip hooks).
  void (*clear_clips)(void* skeleton) = nullptr;
};

void install(const Natives& natives);

// An entity's character and its ISkeletonAnim; null without a character.
void* character_of(void* entity);
void* skeleton_of(void* entity);

// How a clip of a character's animation set travels, "" for none: a blend space by its name
// (without ".bspace"), a plain clip (which a release build keeps no name of) by "#<id>".
std::string clip_name(void* character, std::int32_t clip);

// The id of a clip in a character's animation set by how it travels (clip_name), -1 for none.
std::int32_t clip_id(void* character, const std::string& name);

// The newest clip on a skeleton's layer 0 (-1 none) and its start flags.
std::int32_t top_clip(void* skeleton, std::uint32_t& flags);

// The normalized time of the newest clip on an entity's layer 0, 0 for none.
float top_clip_time(void* entity);

// A motion parameter's value in the newest clip on a skeleton's layer 0 (what its blend space
// plays now); false while that clip has none of that id.
bool clip_parameter(void* skeleton, int id, float& value);

// Runs one frame of the entity's character's animation (its clips' time and pose), as the game
// does for an entity it updates: the character slot's listener for entity event 33
// calls ICharacterInstance slot 19, StartAnimationProcessing(params) with the character's world
// location. A level entity on another client gets no such event (the alchemy table's bellows: the
// owner's game runs them through its own action), so its clip never moves without this. Called
// once a frame while a clip is driven; distance is the camera's, for the character's level of
// detail.
void process(void* entity, float distance);

// The natives, false while they are missing.
bool start(void* skeleton, int clip, std::uint32_t flags, float blend, float key_time);
void stop(void* skeleton);
void clear(void* skeleton);

}  // namespace kcds::runtime::crafting::clips
