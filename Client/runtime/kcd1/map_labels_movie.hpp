// A small movie of the client's own (ActionScript 2), loaded into the game's map screen
// (ApseMap.gfx) on the KCD1 map: a place's label shows no reputation face there.
//
// The map's label class (_global.MapLabel) draws a label in SetData and turns its face
// (this.m_Mc.reputationIcon) to the reputation's frame; every frame is a face. The movie keeps the
// game's SetData under a name of its own and wraps it: after the game's, the face is hidden while
// _root.kcdsMapLabels.hideReputation is true (the client sets it while the KCD1 map is loaded).
// Plain code, no game calls.
#pragma once

#include <cstdint>
#include <vector>

namespace kcds::runtime::kcd1 {

std::vector<std::uint8_t> map_labels_movie();

}  // namespace kcds::runtime::kcd1
