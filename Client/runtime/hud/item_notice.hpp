// The game's own notice of items received or given away, WHGame
// 1.5.5.
//
// The HUD shows it with its item transfer (the HUD events' I_UIItemTransfer, whose first function
// is ShowItemTransfer: class GUID, units, direction 0 received / 1 given), which the game's script
// Game.ShowItemsTransfer reaches through the game's services + 224, then
// + 376. Units the server puts into the player's inventory are not the game's own pick-up, so the
// client shows the notice the server sends with them.
//
// Engine thread.
#ifndef KCDS_RUNTIME_HUD_ITEM_NOTICE_HPP
#define KCDS_RUNTIME_HUD_ITEM_NOTICE_HPP

#include <array>
#include <cstdint>

namespace kcds::runtime::hud {

// game_services: the game's services getter. False when it is missing.
bool install_item_notice(void* game_services);

// Shows the game's notice of units of a class (its 16 bytes as the game keeps them) received or
// given away; false when the HUD is not there.
bool show_item_notice(const std::array<std::uint8_t, 16>& item, std::uint32_t amount, bool gained);

}  // namespace kcds::runtime::hud

#endif  // KCDS_RUNTIME_HUD_ITEM_NOTICE_HPP
