// The local body without gravity while a script animation asks for it (playAnimation gravity:
// false), WHGame 1.5.5.
//
// The game's hanging animations (DeadBody_Hanged, Quest_HangmanDeath) expect its halter, which
// holds the body up by the neck; a player stayed standing on the ground in the hanged pose. While
// such a request runs, the owner's game switches the body's physics off (the entity's physics
// component, slot 24 EnablePhysics, as NoClip does), so the body stays where the script puts it
// (Player.teleport or position, lifted into the noose) and every other client sees it there. When
// the request ends the body is put on the first surface below it (as NoClip lands it) and gets its
// physics back.
//
// Engine thread.
#pragma once

namespace kcds::runtime::animation::no_gravity {

using Logger = void (*)(const char* format, ...);

// physics_component: the entity's physics component getter.
void install(void* physics_component, Logger logger);

// Every frame with the local body: whether the running request has no gravity.
void keep(void* entity, bool wanted);

// Leaving the world: the body that went is forgotten (its physics went with it).
void reset();

}  // namespace kcds::runtime::animation::no_gravity
