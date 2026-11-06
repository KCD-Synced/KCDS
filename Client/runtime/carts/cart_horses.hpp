// The horses in front of the carts: each client spawns its own native C_Horse
// actors for every cart that is harnessed (the actor system's CreateActor with the class
// "Horse", as riding/native_horses does for the synced horses), inside an npcs::OwnSpawn scope
// so the refusal of the game's own animals lets them in. They are nobody's horses and are not
// replicated: every client keeps them on its own cart, which follows the server's pose.
//
// A cart horse is held where its cart puts it every frame (as a replica is) and its legs go with
// the cart: it plays the game's own blend spaces of a harnessed horse by name (npcs::play_clip,
// from Animations/animals/horse/new/blendSpace: 2d_walk_turn_wagon up to 2.3 m/s, 2d_trot_turn
// above, 1d_walk_back_wagon backing up) with their MoveSpeed (motion param 0) and TurnSpeed (1),
// and none while the cart stands, when the horse's own idles play. The movement request carries
// the cart's velocity in place of the horse's own AI requests (remote_inputs, as for a replica).
// Nobody mounts or inspects it: its script's use prompts are switched off (mountableByPlayer and
// inspectableByPlayer, Scripts/Entities/AI/Horse.lua) and riding/native_horses refuses the mount
// of a cart horse.
//
// It wears the game's own cart harness (Libs/Tables/item/clothing_preset.xml): a horse of a pair
// the yoke of its side (horse_cart_doubleLleft: YokeDouble01Left, horse_cart_doubleRight:
// YokeDouble01Right), a horse alone the universal yoke (horse_cart_universal: YokeUniversal01),
// each with the cart bridle (CartBridle01_m01), put on through the game's own equipment as a
// player replica's outfit is (appearance/outfit.hpp).
//
// Its reins and traces go where the game's own cart horses have them, by the game's own pose
// modifier (AnimationPoseModifier_CartHorseReinsModifier,WHGame 1.5.5): the horse's bones
// cartAtt_02, cartAtt_03_hook_L/R and cartAtt_04_hook_L/R_end at the cart's helpers
// whattach_cartAtt_* (the sub-objects of the wagon's axle mesh, wagon_b_axel.cgf, our cart's
// root), and the reins' end, cartAtt_01, in the driver's hand: the left horse's in the left one,
// the right horse's in the right one (joint roles 20 and 21), so both reins are in the one
// driver's hands. Without a driver they lie at the cart's whattach_cartAtt_01. The modifier is
// made by the game's factory, its bone pairs for the horse's side filled the first
// time by the game's own builder (from the table), and pushed on the
// horse's skeleton every frame with the driver, as the game's cart horse step does
// (that step finds the skeleton through an actor slot our horses do not fill, so we push it).
//
// Engine thread.
#pragma once

#include <cstdint>

namespace kcds::game_client {

// Where a cart horse goes in the harness.
enum class HarnessSide { Alone, Left, Right };

// Spawns the cart horse of that name when there is none, then holds it at the world position
// and rotation (x, y, z, w), walks it with the velocity (metres a second) and turn (radians a
// second, to the left) and keeps its harness on. False while it could not be spawned.
// cart_root: the cart's root entity (its axle mesh, with the reins' helpers); driver: the network
// id of the player driving it (0 for none).
bool keep_cart_horse(const char* name, const float position[3], const float rotation[4],
                     const float velocity[3], float turn_rate, HarnessSide side,
                     const char* cart_root, std::uint32_t driver, const float reins_rest[6]);

// Puts every cart horse's reins modifier on its skeleton, before the game's update (core/
// engine_loop): pushed after it, the horse's animation (already running) took it on some frames
// only and the reins flickered (the driver's view).
void push_cart_reins();

// Finds the game's reins modifier natives; false when they are missing (no reins then).
bool install_cart_reins();

// Removes the cart horse of that name.
void remove_cart_horse(const char* name);

// True for the actor of a cart horse.
bool cart_horse_actor(void* actor);

// Forgets every cart horse, as the level does when it unloads (their bodies go with it).
void reset_cart_horses();

}  // namespace kcds::game_client
