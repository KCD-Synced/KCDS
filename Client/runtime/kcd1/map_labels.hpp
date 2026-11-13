// The names on the KCD1 map's world map (its towns and villages, as KCD1's map shows them).
//
// KCD2's map screen shows the labels of its label table (Libs/Tables/ui/ui_map_label.xml: a
// location, a place in the map's pixels from its top left corner, a text key) whose location
// (Libs/Tables/rpg/location.xml) is on the current level. The game also loads a table's patches
// (<table>__<name>.xml, wh_sys_AutoPatchDB): these are made from KCD1's own label table, its places
// on KCD1's level (level 0 of the level table, map_screen.hpp), its names from KCD1's texts.
// Plain code, no game calls.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace kcds::runtime::kcd1 {

// The tables' patch files, by their names in the game's data.
inline constexpr const char* kLocationPatch = "Libs/Tables/rpg/location__kcds_kcd1.xml";
inline constexpr const char* kLabelPatch = "Libs/Tables/ui/ui_map_label__kcds_kcd1.xml";

// The location table's patch: a location on level 0 for each of KCD1's labels with a place.
std::string location_patch(const std::string& kcd1_labels);

// The label table's patch: KCD1's labels at their places on the map (KCD1 keeps them in metres
// of its 4096 m world, the map has 2 pixels a metre, its rows from the north).
std::string label_patch(const std::string& kcd1_labels);

// The names the labels and their locations show, by their keys ("kcds_kcd1_location_rataje"),
// from a KCD1 text table (text_ui_quest.xml rows: key, English, the language's text).
std::vector<std::pair<std::string, std::string>> label_texts(const std::string& kcd1_labels,
                                                             const std::string& kcd1_texts);

}  // namespace kcds::runtime::kcd1
