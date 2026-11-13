// KCD2's showroom in the KCD1 map: the room every KCD2 level keeps far off its map (its editor
// layer "showroom"), where the game's menus show the player, their horse and their dog (the
// inventory's figure, InventoryDummyPlayer; the menus' link, UIApseLinkNode; the shop's,
// UIShopLinkNode) with the room's cameras, lights and panels. Without it the inventory, the
// character sheet and the shops of the game's menu do not open. Plain code, no game calls.
#pragma once

#include <cstddef>
#include <string>

namespace kcds::runtime::kcd1 {

// The showroom's entities of a KCD2 level's objects_mission0.xml, as entities of the KCD1 map's:
// new ids from `next_id` on (their parents and links with them), KCD2's GUIDs and places.
// `count` gets how many there are.
std::string showroom(const std::string& kcd2_objects, long& next_id, std::size_t& count);

}  // namespace kcds::runtime::kcd1
