// The maps one server runs at once: the level first, then the listed maps.
#include "../local/maps/server_maps.hpp"

#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace maps = kcd2s::local::maps;

KCD2S_TEST(server_maps, the_level_alone_is_one_map) {
  std::string problem;
  KCD2S_CHECK(maps::server_maps("trosecko", "", problem) == std::vector<std::string>{"trosecko"});
  KCD2S_CHECK(problem.empty());
}

KCD2S_TEST(server_maps, the_level_comes_first_and_each_map_once) {
  std::string problem;
  const auto listed = maps::server_maps("kutnohorsko", "trosecko, kutnohorsko,trosecko", problem);
  KCD2S_CHECK(listed == (std::vector<std::string>{"kutnohorsko", "trosecko"}));
  KCD2S_CHECK_EQ(maps::map_index(listed, "trosecko"), 1);
  KCD2S_CHECK_EQ(maps::map_index(listed, "klaster"), -1);
}

KCD2S_TEST(server_maps, a_map_the_game_does_not_have_stops_the_server) {
  std::string problem;
  KCD2S_CHECK(maps::server_maps("trosecko", "kutnohorsko,../etc", problem).empty());
  KCD2S_CHECK(problem.find("../etc") != std::string::npos);
}

KCD2S_TEST(server_maps, a_player_on_another_map_gets_its_world_under_a_new_revision) {
  kcd2s::integration::WorldConfig base;
  base.map = "trosecko";
  base.reveal_map = false;
  const auto other = maps::world_on_map(base, "kutnohorsko", 2);
  KCD2S_CHECK_EQ(other.map, std::string("kutnohorsko"));
  KCD2S_CHECK_EQ(other.revision, 2u);
  KCD2S_CHECK(!other.reveal_map);
  KCD2S_CHECK(kcd2s::integration::valid_world_config(other));
}
