// The level's layers as its files list them (Data/Levels/<level>/level.pak): leveldata.xml names
// every layer with its id (<Layer Name="beforeSiege_cf5fd30a-..." Id="1203" ... />), and each
// layer with entities has its own file, layers/<name>.xml, whose entities carry the layer's place
// in the map's tree (EditorLayer="Main/tneb_nebakov/fortress/series_Predhradi/beforeSiege") and
// where they stand (Pos="x,y,z"). A layer is one state of a part of the map in the story (a
// fortress before and after its siege, a forge ruined and rebuilt); the game switches them as the
// story goes on. Plain code, no game calls.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kcds::runtime::level::layers {

struct CatalogLayer {
  std::uint16_t id = 0;
  std::string name;  // the full name the game knows it by, with its GUID
};

// The layers leveldata.xml lists, in its order.
std::vector<CatalogLayer> parse_level_layers(std::string_view leveldata);

// A layer's name without the GUID the level adds ("beforeSiege_cf5fd30a-16c1-49d6-be2e-
// eebaa2bec480" -> "beforeSiege"); the name itself when it has none.
std::string short_layer_name(std::string_view name);

// What a layer's own file says of it.
struct LayerPlace {
  std::string path;      // its place in the map's tree (the first entity's EditorLayer), or empty
  float centre[3] = {};  // the middle of its entities' places
  std::size_t entities = 0;
};

// Reads layers/<name>.xml; nullopt when it holds no entity with a place.
std::optional<LayerPlace> parse_layer_file(std::string_view xml);

}  // namespace kcds::runtime::level::layers
