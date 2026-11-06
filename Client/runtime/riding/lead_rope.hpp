// The rope a led horse walks on (Horse.follow with rope: true), WHGame 1.5.5.
//
// Every client draws it the same way, each frame, from under the horse's head (its Jaw joint, where
// a halter's lead ring is) to its leader's right hand (a player's, a replica's or an NPC's body):
// kSegments pieces of the game's plain cylinder in the rope material, each turned
// along its stretch of a hanging curve and stretched to it. The rope is a little longer than the
// distance the horse keeps (3.2 m), so it hangs while the horse waits and pulls straight as the
// leader walks off; how far it hangs follows smoothly. Not physical: nothing collides with it.
// Live: drawn from the horse's "head" joint to the leader's RightHand in both games;
// from the jaw.
//
// Engine thread.
#pragma once

namespace kcds::game_client {

// Places every led horse's rope for this frame and takes away the ropes of horses no longer led.
void tick_lead_ropes(double now);

// Takes every rope away (the world goes).
void clear_lead_ropes();

}  // namespace kcds::game_client
