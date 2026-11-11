// kcds://icons/item/<name or GUID> in resource web views: an item's inventory icon as a .png.
//
// The web host turns such a load into a file request of the icons kind (kcds::web::kIconRequests,
// path "item/<name or GUID>.png"). The network thread hands it here; the engine thread finds the
// item's icon name (inventory/item_types: a game item's name or GUID, a registered type's id or
// GUID, which answers its own icon or its visual's), reads
// Libs/UI/Textures/Icons/Items/<icon>_icon.dds through the game's file system, so a resource's
// replacing icon and a registered type's mounted icon are what the page shows too, and answers
// the load with the picture as a .png, or a 404.
#pragma once

#include <string>

namespace kcds::runtime::web::icons {

// Network thread: a waiting load to answer.
void queue_request(int id, const std::string& path);

// Engine thread, every frame: answers a few waiting loads.
void answer_requests();

}  // namespace kcds::runtime::web::icons
