// Held props of remote players: what a player holds that the game does not show on
// another machine by itself, such as the book a resource opened in their hands.
//
// The owner reports its prop in the replicated NetworkPlayer field heldProp (an id below, 0 for
// none). Every other client shows it on that player's replica (named kcd2s_remote_<player>): a
// BasicEntity with the prop's model, spawned, animated and removed through the game's natives
// (prop_entity.hpp, no script), then attached to the RightHand joint of the replica
// while it is taken out and put away, and where the game's reading holds a document while it is
// read (CEntity::AttachChild), on the engine thread. It plays the document clips that
// go with the reading clips of the body (take out, read, turn a page, put away).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::props {

struct Natives {
  void* (*entity_system)() = nullptr;  // IEntitySystem
  void* attach_child = nullptr;
  void* (*local_entity)() = nullptr;  // the local player's engine entity
  void* spawn_params_init = nullptr;  // SEntitySpawnParams::SEntitySpawnParams
  void* physics_component = nullptr;  // the entity's physics component
  void* const* engine_3d = nullptr;   // I3DEngine (its material manager)
  void (*log)(const char* format, ...) = nullptr;
};

void install(const Natives& natives);

enum HeldProp : std::int64_t {
  kNone = 0,
  kBookRed = 1,
  kBookPlain = 2,
  kLetter = 3,
};

// Engine thread: the replicated heldProp of a remote player, every update.
void observe(std::uint32_t player, std::int64_t prop);

// Engine thread: the replica of player is gone; its prop goes with it.
void forget(std::uint32_t player);

// Engine thread: the world was reset; nothing of the old one remains.
void reset();

// Engine thread, every frame in a world: attaches the props spawned since the last frame.
void frame();

// Engine thread: the local reader's own document outside the game's reading (the game's
// inventory reading has no take-out or put-away): taken out of the belt into the hand with the
// body (then the game's reading holds its own), put away from the hand, and removed.
void local_take_out(std::int64_t prop);
void local_put_away(std::int64_t prop);
void local_remove();

}  // namespace kcds::runtime::props
