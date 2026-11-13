// The game's map screen movie (Libs/UI/ApseMap.gfx) with no reputation face on its labels, made
// from the player's own copy of the game when KCDS starts (never shipped).
//
// A label (the movie's symbol mc_label) draws its place's reputation as a face: its child
// reputationIcon, a sprite of faces only, turned to the reputation's frame. KCD1's places have
// none of KCD2's factions, so every name on the KCD1 map showed the worst face. The movie made
// here places the movie's own empty clip (mc_empty) as reputationIcon instead: the label's script
// still finds it and turns it, and nothing is drawn. KCD2's maps show their names on their tiles
// and no labels with faces, so they look as before. Plain code, no game calls.
#pragma once

#include <cstdint>
#include <vector>

namespace kcds::runtime::kcd1 {

// The map screen's movie by its name in the game's data (in IPL_GameData.pak).
inline constexpr const char* kMapMovie = "Libs/UI/ApseMap.gfx";

// The movie (a Scaleform GFX or CFX file, or a SWF) uncompressed with its labels' reputationIcon
// an empty clip; empty when the movie is not one this knows (no mc_empty, no such label).
std::vector<std::uint8_t> map_movie_without_faces(const std::vector<std::uint8_t>& movie);

}  // namespace kcds::runtime::kcd1
