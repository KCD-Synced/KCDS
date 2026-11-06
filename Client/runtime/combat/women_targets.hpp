// Women players are lock-on targets: a man's lock-on never
// took a woman player, while hers took him.
//
// The game leaves a body out of the lock-on's candidates when its soul is a woman's
// (the lock-on's exclusion check; its soul slot 42, is HasGender: the
// archetype's gender byte, soul + 3256 -> + 28, against the argument, here 2). In KCD2 women are
// not fought, but another player's woman is a player. During that check alone, a soul of another
// player's replica answers HasGender(2) with false; the game's women stay out, and nothing else
// (clothing, voice, animation) sees another gender.
#pragma once

namespace kcds::game_client {

// Hooks the exclusion check and HasGender; false (logged) when either is not found. MinHook must
// be initialized.
bool install_women_targets();

}  // namespace kcds::game_client
