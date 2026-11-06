#include "kcd2s/integration/nameplates.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// A player's nameplate as the server keeps it.
KCD2S_TEST(nameplates, field_round_trip) {
  NameplateState state;
  KCD2S_CHECK(nameplate_field(state).empty());
  NameplateState decoded;
  decoded.visible = false;
  KCD2S_CHECK(parse_nameplate_field("", decoded) && decoded == NameplateState{});
  state.visible = false;
  state.health_visible = false;
  state.color = 0xFFC9A227u;
  state.text = "[Guard] Hans; Jindřich";
  KCD2S_CHECK(nameplate_field(state) == "00;FFC9A227;[Guard] Hans; Jindřich");
  KCD2S_CHECK(parse_nameplate_field(nameplate_field(state), decoded) && decoded == state);
  state = NameplateState{};
  state.health_visible = false;
  KCD2S_CHECK(parse_nameplate_field(nameplate_field(state), decoded) && decoded == state);
}

KCD2S_TEST(nameplates, bad_fields_refused) {
  NameplateState decoded;
  KCD2S_CHECK(!parse_nameplate_field("1", decoded));
  KCD2S_CHECK(!parse_nameplate_field("21;FFFFFFFF;", decoded));
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFG;", decoded));
  KCD2S_CHECK(!parse_nameplate_field("11;ffffffff;", decoded));
  KCD2S_CHECK(!parse_nameplate_field("11:FFFFFFFF;", decoded));
  // A line break starts the title; any other is refused.
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFF;a\rb", decoded));
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFF;" + std::string(kMaxNameplateTextBytes + 1, 'a'),
                                     decoded));
  KCD2S_CHECK(
      parse_nameplate_field("11;FFFFFFFF;" + std::string(kMaxNameplateTextBytes, 'a'), decoded));
}

// Only the health bar (Player.setNameplateMode('health')): a third flag for the hidden name, the
// two-flag form for every tag that shows it.
KCD2S_TEST(nameplates, hidden_name_field_round_trip) {
  NameplateState state;
  state.name_visible = false;
  KCD2S_CHECK(nameplate_field(state) == "110;FFFFFFFF;");
  NameplateState decoded;
  KCD2S_CHECK(parse_nameplate_field(nameplate_field(state), decoded) && decoded == state);
  state.text = "Hans";
  state.health_visible = false;
  KCD2S_CHECK(parse_nameplate_field(nameplate_field(state), decoded) && decoded == state);
  KCD2S_CHECK(parse_nameplate_field("10;FFFFFFFF;", decoded) && decoded.name_visible);
  KCD2S_CHECK(parse_nameplate_field("111;FFFFFFFF;", decoded) && decoded.name_visible);
  KCD2S_CHECK(!parse_nameplate_field("112;FFFFFFFF;", decoded));
  KCD2S_CHECK(!parse_nameplate_field("110;FFFFFFFF", decoded));
}

// A title above the name (Player.setNameplateTitle): a line of its own after the text, with its
// colour (0: the plate's gold) and size (0: the plate's own).
KCD2S_TEST(nameplates, title_field_round_trip) {
  NameplateState state;
  state.title = "Town guard";
  KCD2S_CHECK(nameplate_field(state) == "11;FFFFFFFF;\n00000000;0;Town guard");
  NameplateState decoded;
  KCD2S_CHECK(parse_nameplate_field(nameplate_field(state), decoded) && decoded == state);
  state.text = "Hans";
  state.name_visible = false;
  state.title = "Admin";
  state.title_color = 0xFFE0402Au;
  state.title_size = 30;
  KCD2S_CHECK(nameplate_field(state) == "110;FFFFFFFF;Hans\nFFE0402A;30;Admin");
  KCD2S_CHECK(parse_nameplate_field(nameplate_field(state), decoded) && decoded == state);
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFF;Hans\n", decoded));
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFF;Hans\nAdmin", decoded));
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFF;Hans\n00000000;Admin", decoded));
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFF;Hans\n00000000;0;", decoded));
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFF;Hans\n00000000;0;A\nB", decoded));
  // Sizes outside 8 to 48 (0 is the plate's own).
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFF;Hans\n00000000;7;Admin", decoded));
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFF;Hans\n00000000;49;Admin", decoded));
  KCD2S_CHECK(!parse_nameplate_field("11;FFFFFFFF;Hans\n00000000;100;Admin", decoded));
  KCD2S_CHECK(parse_nameplate_field("11;FFFFFFFF;Hans\n00000000;48;Admin", decoded) &&
              decoded.title_size == 48);
  KCD2S_CHECK(!parse_nameplate_field(
      "11;FFFFFFFF;\n00000000;0;" + std::string(kMaxNameplateTextBytes + 1, 'a'), decoded));
}
