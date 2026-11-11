#include "../layer_catalog.hpp"

#include <cmath>

#include "kcd2s/testkit.hpp"

namespace layers = kcds::runtime::level::layers;

KCD2S_TEST(layer_catalog, the_level_data_names_every_layer_with_its_id) {
  const std::string leveldata = R"(<Level>
  <Layers>
    <Layer Name="_common_0008c0ca-3b0c-4b6d-9037-0b2b6b187f51" Id="1" Specs="-1" />
    <Layer Name="beforeSiege_cf5fd30a-16c1-49d6-be2e-eebaa2bec480" Id="1203" Specs="-1" Physics="1" />
    <Layer Id="7" Name="kovar_4158a7b9-ddd5-492e-ba02-2dd8c124cd80" />
    <Layer Name="" Id="9" />
    <Layer Name="broken" Id="70000" />
  </Layers>
</Level>)";
  const auto found = layers::parse_level_layers(leveldata);
  KCD2S_CHECK_EQ(found.size(), std::size_t{3});
  KCD2S_CHECK_EQ(found[1].id, std::uint16_t{1203});
  KCD2S_CHECK(found[1].name == "beforeSiege_cf5fd30a-16c1-49d6-be2e-eebaa2bec480");
  KCD2S_CHECK_EQ(found[2].id, std::uint16_t{7});
}

KCD2S_TEST(layer_catalog, a_short_name_drops_only_a_whole_guid) {
  KCD2S_CHECK(layers::short_layer_name("beforeSiege_cf5fd30a-16c1-49d6-be2e-eebaa2bec480") ==
              "beforeSiege");
  KCD2S_CHECK(layers::short_layer_name(
                  "cin_m0930t_utoknebakov__after_duel_085708e1-be43-11eb-8e1b-fcaa140428b5") ==
              "cin_m0930t_utoknebakov__after_duel");
  KCD2S_CHECK(layers::short_layer_name("kovar") == "kovar");
  KCD2S_CHECK(layers::short_layer_name("odd_cf5fd30a-16c1-49d6-be2e-eebaa2bec48z") ==
              "odd_cf5fd30a-16c1-49d6-be2e-eebaa2bec48z");
}

KCD2S_TEST(layer_catalog, a_layer_file_gives_its_place_in_the_tree_and_its_middle) {
  const std::string xml = R"(<Objects>
	<Entity Name="ItemSlot177" Pos="1987,1161,55" EntityClass="ItemSlot" EditorLayer="Main/tneb_nebakov/fortress/series_Predhradi/beforeSiege">
		<Properties nDestockPeriodDays="20" />
	</Entity>
	<Entity Name="Fire" Pos="1991,1165,57" EditorLayer="Main/tneb_nebakov/fortress/series_Predhradi/beforeSiege" />
	<Entity Name="NoPlace" EditorLayer="Main/other" />
</Objects>)";
  const auto place = layers::parse_layer_file(xml);
  KCD2S_CHECK(place.has_value());
  if (!place) {
    return;
  }
  KCD2S_CHECK(place->path == "Main/tneb_nebakov/fortress/series_Predhradi/beforeSiege");
  KCD2S_CHECK_EQ(place->entities, std::size_t{2});
  KCD2S_CHECK(std::fabs(place->centre[0] - 1989.0f) < 1e-3f);
  KCD2S_CHECK(std::fabs(place->centre[1] - 1163.0f) < 1e-3f);
  KCD2S_CHECK(std::fabs(place->centre[2] - 56.0f) < 1e-3f);
  KCD2S_CHECK(!layers::parse_layer_file("<Objects />").has_value());
}
