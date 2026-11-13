// KCD1's dice tables as KCD2's. Plain code, no game calls.
//
// A dice table in either game is a prefab of entities around a table: two seats, the cups'
// slots, the cup, a light. KCD2's has more (its DiceInteractor, which offers "Play dice", the
// smart object that runs the match, the dice's slots and the camera's points), and KCDS's dice
// (the server's Dice module and the client's dice code) play on those. KCD1's tables are the same
// size (1.40 m between the seats in both), so each of KCD1's gets the entities of one of KCD2's
// tables (klaster's, kDiceTemplateInteractor's prefab): turned and moved so that its two seats are
// where KCD1's two seats are, with new entity ids and GUIDs. KCD1's own dice entities are left out
// (entities.hpp), its table, chairs and light stay.
//
// A placed entity's GUID is dice_table_guid(instance, template GUID), where the instance is the
// GUID in KCD1's prefab names (dice_cup_<instance>, seat1_<instance>...): the server's tool
// (Dev/tools/dice/build_kcd1_tables.py) names the tables by the same GUIDs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kcds::runtime::kcd1 {

// The DiceInteractor (its EntityGuid) of the KCD2 table every KCD1 table is made of.
inline constexpr const char* kDiceTemplateInteractor = "0679658d-a25d-0a80";

// The GUID (sixteen lowercase hex digits) of the entity of KCD1 table `instance` made from
// template entity `template_guid` (sixteen hex digits, dashes left out): FNV-1a 64 of
// "<instance>/<template_guid>", both in lower case.
std::string dice_table_guid(const std::string& instance, const std::string& template_guid);

// KCD2's entities of every KCD1 dice table of `kcd1_objects`, made from the template table of
// `kcd2_objects` (klaster's objects_mission0.xml); empty when either has none. The entities take
// ids from `next_id` up; `tables` counts the tables.
std::string dice_tables(const std::string& kcd1_objects, const std::string& kcd2_objects,
                        long& next_id, std::size_t& tables);

}  // namespace kcds::runtime::kcd1
