// What the studio's character wears: the editor's outfit, put on through KCDS's
// outfit code (appearance/outfit.hpp apply: pieces not listed come off and out of the inventory,
// missing ones are created and put on, through the game's own equipment).
//
//   { op: 'studioOutfit', pieces: ['<32 hex digits>', ...] }   from the editor: each piece's
//                                                              item class in the scripts' form
//                                                              (catalog.cjs classBytesHex: two
//                                                              little-endian 64-bit numbers,
//                                                              integration/hands.hpp); [] for none
//
// The game's item database is loaded in the studio's start too (5604 classes in its registry,
// C_ItemDatabase); the game makes each piece by its class
// there (->).
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::outfit {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world: the local body wears the editor's outfit.
void tick(void* local_actor, void* local_entity);

}  // namespace kcds::runtime::studio::outfit
