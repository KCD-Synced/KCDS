// A player's own tack changes on a server horse, made in the game's inventory screen,
// Horse tab (the horse's doll: Equip / Unequip).
//
// The server holds what a server horse wears (the NetworkHorse field `gear`, which every client
// dresses the horse in, appearance/outfit.hpp). A change the player makes in the Horse tab does
// not happen in the game: it is asked of the server, which takes the piece put on out of the
// player's inventory and gives back what came off, or refuses (gearFixed, horseGearChanging).
// Its answer is the horse's new gear, which the outfit sync then puts on.
//
//   - the inventory screen's Horse tab sets its own flag (the screen + 2752) on its messages
//     goInHorseTab / goOutHorseTab (hooked; it also turns the quick slots' actions
//     off and on): a change counts as the player's only while that tab is open and a game menu
//     shows (the flag stays set when the screen closes);
//   - the soul equipment manager's equip (manager, item, flag; the equipment
//     interface's slot 40 is a thunk to it, `mov rcx,[rcx+120h]; jmp`) and unequip
//     (manager, item, 1, reason, flag; slot 41's thunk) are hooked: on a server horse's manager,
//     an equip of an item of the horse's saddlebags or of the player's inventory (its row) or an
//     unequip (its class) goes to the server and the game is refused (equip: 0, the game's own
//     refusal; unequip: 1, as the game always answers, with nothing taken off);
//   - the outfit sync's own equips and unequips (while_syncing) always go through.
//
// Engine thread.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace kcds::runtime::riding::horse_tack {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* equip = nullptr;      // hooked
  void* unequip = nullptr;    // hooked
  void* horse_tab = nullptr;  // hooked
  // The server horse whose soul's equipment manager this is, or 0.
  std::uint32_t (*horse_of_manager)(void* manager) = nullptr;
  // Whether a game menu shows (the inventory screen is one).
  bool (*menu_open)() = nullptr;
  // The player's row of an item of their inventory, by its handle; false for none.
  bool (*row_of)(std::uint64_t handle, std::string& row) = nullptr;
  // The row of the horse's saddlebags an item of its inventory shows, by its handle; false for
  // none (riding/horse_saddlebags.hpp).
  bool (*bags_row_of)(std::uint32_t horse, std::uint64_t handle, std::string& row) = nullptr;
  // Sends the owner's equip of a row (of the saddlebags with from_bags, else their own), or
  // removal of a class, to the server; false when it could not.
  bool (*send_equip)(std::uint32_t horse, const std::string& row, bool from_bags) = nullptr;
  bool (*send_remove)(std::uint32_t horse,
                      const std::array<std::uint8_t, 16>& item_class) = nullptr;
};

// Hooks the three natives; false when one is missing or a hook fails.
bool install(const Natives& natives, Logger logger);

// The outfit sync dresses a horse: its equips and unequips are its own, never the player's.
class WhileSyncing {
 public:
  WhileSyncing();
  ~WhileSyncing();
  WhileSyncing(const WhileSyncing&) = delete;
  WhileSyncing& operator=(const WhileSyncing&) = delete;
};

// Each frame: the Horse tab counts as closed once no game menu shows.
void tick();

}  // namespace kcds::runtime::riding::horse_tack
