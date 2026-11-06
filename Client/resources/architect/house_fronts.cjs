'use strict';

// The street front of the game's city houses (house_city_*): the side of the house's box, +y or
// -y, whose outer wall holds its street doors. Read from the houses' own prefabs (IPL_GameData.pak
// Prefabs/structures/living/houses/house_city_*.xml: the nested Door/door_village_* objects 1.5 to
// 4.5 m inside that side of the box, the box World Architect gives them). Houses with doors on
// both or neither of those sides are left out. A house turned 0 degrees has its +y front facing
// north.
const HOUSE_FRONTS = Object.freeze({
  'structures/living/houses/house_city_2s_arc_a': '+y',
  'structures/living/houses/house_city_2s_arc_a_platnerska06': '+y',
  'structures/living/houses/house_city_2s_arc_a_radnicni13': '+y',
  'structures/living/houses/house_city_2s_arc_a_radnicni26': '+y',
  'structures/living/houses/house_city_2s_arc_b': '-y',
  'structures/living/houses/house_city_2s_arc_b_platnerska05': '-y',
  'structures/living/houses/house_city_2s_arc_c': '+y',
  'structures/living/houses/house_city_2s_arc_c_platnerska04': '+y',
  'structures/living/houses/house_city_2s_noarc_a_tarmark15': '-y',
  'structures/living/houses/house_city_2s_noarc_b': '-y',
  'structures/living/houses/house_city_2s_noarc_d_radnicni06': '-y',
  'structures/living/houses/house_city_2s_noarc_e': '-y',
  'structures/living/houses/house_city_2s_noarc_e_tarmark02': '-y',
  'structures/living/houses/house_city_2s_noarc_f': '+y',
  'structures/living/houses/house_city_2s_noarc_g': '+y',
  'structures/living/houses/house_city_3s_arc_a': '+y',
  'structures/living/houses/house_city_3s_arc_a_kupecka03': '+y',
  'structures/living/houses/house_city_3s_arc_a_leflirska02': '+y',
  'structures/living/houses/house_city_3s_arc_b': '+y',
});

module.exports = { HOUSE_FRONTS };
