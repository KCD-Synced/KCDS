// Which horse the local player's game holds as the player's own horse.
//
// User decision: "When is horse given to player its not
// steal its your horse". The game has one such horse per player, the horse it calls the player's
// (Pebbles in the campaign): the horse's Lua actions offer a plain mount on a short press for it
// and "mount and steal", held, for any other horse (Scripts.pak Horse.lua GetActions:
// player:GetHorseId() == self.id). The server gives horses with Horse.giveTo (NetworkHorse
// field horseOwner); riding/player_horse.hpp makes one of them the game's player horse.
//
// This is the choice alone, without the game: every frame the horses the server gives the local
// player and whose bodies are ready are offered, then decide() says what to do. The horse already
// held stays while it is still given; otherwise the newest given horse (the highest network id)
// is taken. A horse the game refused is not asked again until another horse is chosen. Engine
// thread.
#ifndef KCDS_RUNTIME_RIDING_OWNED_HORSE_HPP
#define KCDS_RUNTIME_RIDING_OWNED_HORSE_HPP

#include <cstdint>

namespace kcds::runtime::riding {

class OwnedHorse {
 public:
  enum class Step { Keep, Make, Clear };

  // A horse the server gives the local player, with its body in this game.
  void offer(std::uint32_t horse) {
    if (horse == 0) {
      return;
    }
    if (horse == held_) {
      held_offered_ = true;
    }
    if (horse > newest_) {
      newest_ = horse;
    }
  }

  // After this frame's offers: Make wanted(), Clear the held horse, or Keep. The offers start
  // again for the next frame.
  Step decide() {
    wanted_ = held_offered_ ? held_ : newest_;
    held_offered_ = false;
    newest_ = 0;
    if (wanted_ != refused_) {
      refused_ = 0;
    }
    if (wanted_ == held_) {
      return Step::Keep;
    }
    if (wanted_ == 0) {
      return Step::Clear;
    }
    // A refusal left nothing held; the game is not asked again for the same horse.
    if (wanted_ == refused_) {
      return Step::Keep;
    }
    return Step::Make;
  }

  // The horse decide() chose.
  std::uint32_t wanted() const {
    return wanted_;
  }

  // The outcome of a Make. A refusal leaves the game without a player horse (the game lets the
  // old one go before it takes the new one).
  void made(std::uint32_t horse, bool accepted) {
    held_ = accepted ? horse : 0;
    refused_ = accepted ? 0 : horse;
  }

  // The game holds no player horse of ours any more (a Clear, or the held horse's body left).
  void cleared() {
    held_ = 0;
  }

  // The horse's body leaves this game.
  void forget(std::uint32_t horse) {
    if (horse == held_) {
      held_ = 0;
    }
    if (horse == refused_) {
      refused_ = 0;
    }
  }

  std::uint32_t held() const {
    return held_;
  }

  // A new world: nothing is held.
  void reset() {
    *this = OwnedHorse{};
  }

 private:
  std::uint32_t held_ = 0;
  std::uint32_t refused_ = 0;
  std::uint32_t wanted_ = 0;
  std::uint32_t newest_ = 0;
  bool held_offered_ = false;
};

}  // namespace kcds::runtime::riding

#endif  // KCDS_RUNTIME_RIDING_OWNED_HORSE_HPP
