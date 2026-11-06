#pragma once

#include <cstdint>

namespace kcds::runtime::combat {

// The ordinary attack a replica's attack factory made right after the game's own pair (sync)
// attack choice for that replica was refused. Only the owner's game chooses pairs (its
// choice arrives as a SyncHit owner selection), so a replica's choice is refused
// (hooked_choose_sync_attack). The factory then makes the ordinary attack from a
// row the game marked for the pair it wanted (attack + 98), and the attack request
// check right after it refuses an attack with that mark: the replica did not swing
// where its owner's game made the ordinary attack. Only that attack, checked right after the
// factory call that made it, loses the mark; any other factory call or check in between drops the
// record, so a later attack of the same replica keeps its native mark. Engine thread; addresses
// are local and never travel.
class RefusedPair {
 public:
  // An attack factory call starts for `movement` (any fighter's).
  void factory_started(std::uintptr_t movement) {
    movement_ = movement;
    refused_ = false;
    action_ = 0;
  }

  // The game's pair choice for `movement` was refused inside that call.
  void pair_refused(std::uintptr_t movement) {
    if (movement && movement == movement_) {
      refused_ = true;
    }
  }

  // The factory call for `movement` made `attack`, an ordinary attack (0 for none or another kind).
  void factory_made(std::uintptr_t movement, std::uintptr_t attack) {
    action_ = refused_ && movement && movement == movement_ ? attack : 0;
    movement_ = 0;
    refused_ = false;
  }

  // The request check for `attack`: true when it is the attack made after the refused choice.
  bool take(std::uintptr_t attack) {
    const bool refused = attack && attack == action_;
    action_ = 0;
    return refused;
  }

 private:
  std::uintptr_t movement_ = 0;
  bool refused_ = false;
  std::uintptr_t action_ = 0;
};

}  // namespace kcds::runtime::combat
