#include "dropped_weapons.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>

#include "../ground/ground_items.hpp"
#include "../inventory/stock_projection.hpp"
#include "../weapons/drawn_weapons.hpp"

namespace kcds::runtime::knockout::dropped_weapons {

namespace {

// An item's handle (its WUID, item + 48: weapons/drawn_weapons.hpp).
constexpr std::size_t kItemHandle = 48;

struct Hand {
  std::uint64_t handle = 0;
  // The owner's row of the item (the local player's only), read while it was still held.
  std::string row;
};

struct Body {
  // What the hands held in the last two frames the body stood (right, left; then the frame
  // before): the game may let the weapons go in the frame before this one sees the body down.
  std::array<Hand, 4> held;
  // What the hands held as the body went down, until each is let go of.
  std::array<Hand, 4> falling;
  bool down = false;
  std::chrono::steady_clock::time_point down_at{};
};

// How long the owner's game may keep a lying body's weapon in its hand before the client drops
// it itself (after a knock-out it stayed equipped in the hand, nobody could take it up).
constexpr auto kOwnLetGo = std::chrono::milliseconds(1000);

std::unordered_map<std::uint32_t, Body> bodies;

std::uint64_t handle_of(void* item) {
  std::uint64_t handle = 0;
  if (item) {
    std::memcpy(&handle, static_cast<const std::uint8_t*>(item) + kItemHandle, sizeof(handle));
  }
  return handle;
}

Hand hand_of(void* actor, bool left, bool own) {
  Hand hand;
  hand.handle = handle_of(weapons::item_in_hand(actor, left));
  std::uint64_t revision = 0;
  if (own && hand.handle && !inventory::row_of(hand.handle, hand.row, revision)) {
    hand.row.clear();
  }
  return hand;
}

bool in_hands(void* actor, std::uint64_t handle) {
  return handle_of(weapons::item_in_hand(actor, false)) == handle ||
         handle_of(weapons::item_in_hand(actor, true)) == handle;
}

}  // namespace

void tick(std::uint32_t key, void* actor, bool down, bool own, Logger logger) {
  if (!actor) {
    return;
  }
  Body& body = bodies[key];
  if (!down) {
    body.down = false;
    body.falling = {};
    body.held[2] = body.held[0];
    body.held[3] = body.held[1];
    body.held[0] = hand_of(actor, false, own);
    body.held[1] = hand_of(actor, true, own);
    return;
  }
  if (!body.down) {
    body.down = true;
    body.down_at = std::chrono::steady_clock::now();
    body.falling = body.held;
    // An item held in both frames is looked for once.
    for (unsigned earlier = 2; earlier < body.falling.size(); ++earlier) {
      const std::uint64_t handle = body.falling[earlier].handle;
      if (handle == body.falling[0].handle || handle == body.falling[1].handle) {
        body.falling[earlier] = Hand{};
      }
    }
  }
  for (Hand& hand : body.falling) {
    if (!hand.handle) {
      continue;
    }
    void* item = ground::item_of_handle(hand.handle);
    if (!item) {
      hand = Hand{};
      continue;
    }
    // A replica's game moves the weapon into the world's inventory as it falls; the owner's game
    // keeps it in its player's inventory while it lies, so there it is let go of when it
    // has left the hands.
    const bool in_world = ground::lies_in_world(item);
    // A replica kept out of the game's ragdoll action may not let go at all: its copy stayed in
    // the lying body's hand, one nobody could take up. It is put away, as
    // human:HolsterWeapon does; the owner's drop lies at the body for everyone.
    if (!own && !in_world && in_hands(actor, hand.handle)) {
      const bool put_away = weapons::holster(actor);
      if (logger) {
        logger("KNOCKOUT replica's weapon still in the lying body's hand: put away=%u",
               unsigned(put_away));
      }
      hand = Hand{};
      continue;
    }
    const bool held_too_long = own && in_hands(actor, hand.handle) &&
                               std::chrono::steady_clock::now() - body.down_at >= kOwnLetGo;
    if (!in_world && !held_too_long && (!own || in_hands(actor, hand.handle))) {
      continue;
    }
    if (own && !hand.row.empty()) {
      ground::ask_drop(hand.row, 1);
    }
    const bool taken = in_world && ground::take_away(item);
    if (logger) {
      logger(
          "KNOCKOUT %s weapon let go as the body fell: in the world=%u taken out=%u, server "
          "drop of row %s",
          own ? "local player's" : "replica's", unsigned(in_world), unsigned(taken),
          own && !hand.row.empty() ? hand.row.c_str() : "-");
    }
    hand = Hand{};
  }
}

void forget(std::uint32_t key) {
  bodies.erase(key);
}

void reset() {
  bodies.clear();
}

}  // namespace kcds::runtime::knockout::dropped_weapons
