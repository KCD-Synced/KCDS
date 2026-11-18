// What players wear on their heads besides hair and clothing (Player.crown, Player.hairCrown;
// the NetworkPlayer field "headPiece", kcd2s/integration/head_pieces.hpp), on the local body and
// on every replica.
//
// A crown is one of the game's own skinned meshes worn on an attachment of our own
// (skin_binding.hpp), so it moves with the head. Sigismund's crown is part of his hair
// (m_hair_zikmund): that mesh is worn with its hair parts not drawn, through a replacement
// material whose parts "haircap" and "haircards" draw nothing (level/model_parts.hpp). A hair
// without its crown is the body's own hair drawn with a material whose part "crown" draws nothing,
// on every skin of the body that has such a part.
//
// A body the game dresses again (a change of clothes or hair) gets new attachment objects, so
// each body is looked at again every second.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::appearance::head_pieces {

// What the player `player` wears by its field; `actor` is the player's body here, or null.
void show(std::uint32_t player, void* actor, const std::string& field, double now);

// Forgets the players not shown this frame.
void keep_only(const std::vector<std::uint32_t>& shown);

}  // namespace kcds::runtime::appearance::head_pieces
