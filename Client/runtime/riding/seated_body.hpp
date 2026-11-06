// A rider stays seated on its horse.
//
// The game keeps an actor's body position (1 standing ... 5 on a horse) in its posture state.
// While a player rode, the game reset that to standing when an unrelated entity near it was
// removed (a level candle's particle effect, a temporary particle): the player's behavior tree
// handles the removal as "the thing you sit on is gone" and sets the body position to standing,
// although the mount state still says mounted. The rider then played the on-foot clips and stood
// on the horse, for itself and for everyone who saw its replica (reproduced on two clients).
//
// The posture state's setter is hooked: while the actor is fully mounted (mount state 2) a body
// position other than the horse one is not taken. Mounting (state 1) and dismounting (state 3,
// then 0) set the body position themselves and are untouched.
#ifndef KCDS_RUNTIME_RIDING_SEATED_BODY_HPP
#define KCDS_RUNTIME_RIDING_SEATED_BODY_HPP

namespace kcds::runtime::riding {

using Logger = void (*)(const char* format, ...);

// The posture state's body position setter (state, body position, second position,
// linked object). False when it is missing or cannot be hooked.
bool keep_seated_riders(void* set_body_position, Logger logger);

}  // namespace kcds::runtime::riding

#endif  // KCDS_RUNTIME_RIDING_SEATED_BODY_HPP
