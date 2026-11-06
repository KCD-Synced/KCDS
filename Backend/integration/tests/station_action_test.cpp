#include "kcd2s/integration/station_action.hpp"

#include <string>

#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// The NetworkPlayer field "stationAction".
namespace {

StationAction smithing() {
  StationAction action;
  action.kind = kStationKindSmith;
  action.station = {1997.125f, 2498.75f, 115.0625f};
  action.workpiece = "horseshoe";
  action.forge = 7;
  action.glow = "80";
  action.effects = kStationForgeHeat | kStationBurning;
  action.sounds = "010000000000000000";  // the forge's sound started once
  action.strokes = 254;
  action.quenches = 1;
  StationProp hammer;
  hammer.id = StationPropId::Hammer;
  hammer.anchor = StationAnchor::RightHand;
  hammer.position = {0.02f, -0.05f, 0.1f};
  hammer.rotation = {0.0f, 0.7071f, 0.0f, 0.7071f};
  StationProp workpiece;
  workpiece.id = StationPropId::Workpiece;
  workpiece.anchor = StationAnchor::Station;
  workpiece.position = {-0.5f, 1.25f, 0.95f};
  action.props = {hammer, workpiece};
  return action;
}

}  // namespace

KCD2S_TEST(station_action, round_trips) {
  const StationAction action = smithing();
  const std::string field = station_action_field(action);
  KCD2S_CHECK(field.rfind("smith S1997.1250,2498.7500,115.0625 Whorseshoe F7 G80 E6 K254 Q1 "
                          "A010000000000000000 HR ",
                          0) == 0);
  StationAction parsed;
  KCD2S_CHECK(parse_station_action_field(field, parsed));
  KCD2S_CHECK(parsed == action);
}

KCD2S_TEST(station_action, without_a_workpiece_or_props) {
  StationAction action = smithing();
  action.workpiece.clear();
  action.props.clear();
  const std::string field = station_action_field(action);
  KCD2S_CHECK(field == "smith S1997.1250,2498.7500,115.0625 F7 G80 E6 K254 Q1 A010000000000000000");
  StationAction parsed;
  KCD2S_CHECK(parse_station_action_field(field, parsed) && parsed == action);
}

KCD2S_TEST(station_action, refuses_what_it_cannot_carry) {
  StationAction action = smithing();
  action.kind = "bake";
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.forge = 10;
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.glow = "12a";
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.glow = "12345";
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.effects = 8;
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.sounds = "0100";
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.sounds = "0100000000000000g0";
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.workpiece = "horse shoe";
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.props[0].position = {3.0f, 0.0f, 0.0f};  // beyond a hand's reach
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.props[1].id = StationPropId::Hammer;  // two hammers
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.props[0].rotation = {0.0f, 0.0f, 0.0f, 0.0f};
  KCD2S_CHECK(station_action_field(action).empty());
}

KCD2S_TEST(station_action, malformed_fields_do_not_parse) {
  StationAction parsed;
  const std::string good = station_action_field(smithing());
  KCD2S_CHECK(!parse_station_action_field("", parsed));
  KCD2S_CHECK(!parse_station_action_field(good + " ", parsed));
  KCD2S_CHECK(!parse_station_action_field(good + " HR", parsed));
  const std::string quiet = std::string(" A") + kStationSoundsNone;
  KCD2S_CHECK(!parse_station_action_field("smith S1,2 F0 G00 E0 K0 Q0" + quiet, parsed));
  KCD2S_CHECK(!parse_station_action_field("smith S1,2,3 F0 G00 E0 K0" + quiet, parsed));
  KCD2S_CHECK(!parse_station_action_field("smith S1,2,3 F0 G00 K0 Q0" + quiet, parsed));
  KCD2S_CHECK(!parse_station_action_field("smith S1,2,3 F0 G00 E0 K0 Q0", parsed));
  KCD2S_CHECK(!parse_station_action_field("smith S1,2,3 F0 G00 E0 K256 Q0" + quiet, parsed));
  KCD2S_CHECK(!parse_station_action_field("smith S1,2,3 F0 G00 E8 K0 Q0" + quiet, parsed));
  KCD2S_CHECK(!parse_station_action_field("smith S1,2,3 F0 G00 E0 K0 Q0 A0", parsed));
  KCD2S_CHECK(!parse_station_action_field(
      "smith S1,2,3 F0 G00 E0 K0 Q0" + quiet + " XR 0,0,0 0,0,0,1", parsed));
  KCD2S_CHECK(!parse_station_action_field(
      "smith S1,2,3 F0 G0 E0 K0 Q0" + quiet + " HX 0,0,0 0,0,0,1", parsed));
  KCD2S_CHECK(parse_station_action_field(
      "smith S1,2,3 F0 G00 E0 K0 Q0" + quiet + " HS 0,0,0 0,0,0,1", parsed));
  KCD2S_CHECK(parsed.props.size() == 1 && parsed.props[0].anchor == StationAnchor::Station);
  // The workpiece carried in the tongs travels relative to them.
  StationAction carried = parsed;
  carried.props[0].anchor = StationAnchor::Tongs;
  StationAction carried_back;
  KCD2S_CHECK(parse_station_action_field(station_action_field(carried), carried_back) &&
              carried_back.props.size() == 1 &&
              carried_back.props[0].anchor == StationAnchor::Tongs);
}

KCD2S_TEST(station_action, zones_glow) {
  StationAction action = smithing();
  action.glow = "00407fff";
  StationAction parsed;
  KCD2S_CHECK(parse_station_action_field(station_action_field(action), parsed));
  KCD2S_CHECK(parsed.glow == "00407fff");
  KCD2S_CHECK(station_glow_level(300.0f) == 0);
  KCD2S_CHECK(station_glow_level(kStationGlowCold + 64.0f * kStationGlowStep) == 64);
  KCD2S_CHECK(station_glow_level(2000.0f) == 255);
  KCD2S_CHECK(station_glow_temperature(0) == kStationGlowCold);
  KCD2S_CHECK(station_glow_temperature(255) == kStationGlowWhite);
  KCD2S_CHECK(station_glow_text(171) == "ab" && station_glow_parse("ab") == 171);
  // An odd count, a capital or a fifth zone is not written.
  action.glow = "0";
  KCD2S_CHECK(station_action_field(action).empty());
  action.glow = "AB";
  KCD2S_CHECK(station_action_field(action).empty());
  action.glow = "0011223344";
  KCD2S_CHECK(station_action_field(action).empty());
}

KCD2S_TEST(station_action, sparks_and_burning_iron) {
  StationAction action = smithing();
  action.sparks_at = true;
  action.sparks = {-0.25f, 1.5f, 0.75f, 2.5f};
  action.burning = {0.0f, 1.25f, 0.5f};
  const std::string field = station_action_field(action);
  KCD2S_CHECK(field.find(" Z-0.2500,1.5000,0.7500,2.5000 B0.0000,1.2500,0.5000 HR ") !=
              std::string::npos);
  StationAction parsed;
  KCD2S_CHECK(parse_station_action_field(field, parsed) && parsed == action);
  // Where the burning effects are: three numbers per effect, or the field is not written.
  action.burning_at = {0.5f, 1.0f, 0.9f, 0.5f, 1.0f, 0.9f, 0.25f, 0.25f, 1.0f};
  const std::string placed = station_action_field(action);
  KCD2S_CHECK(placed.find(" O0.5000,1.0000,0.9000,") != std::string::npos);
  KCD2S_CHECK(parse_station_action_field(placed, parsed) && parsed == action);
  action.burning_at.pop_back();
  KCD2S_CHECK(station_action_field(action).empty());
  action.burning_at.clear();
  action.burning.assign(kStationBurningMax + 1, 1.0f);
  KCD2S_CHECK(station_action_field(action).empty());
  action = smithing();
  action.sparks_at = true;
  action.sparks = {0.0f, 0.0f, 0.0f, 1000.0f};
  KCD2S_CHECK(station_action_field(action).empty());
}

KCD2S_TEST(station_action, bellows_clip) {
  StationAction action = smithing();
  action.burning = {1.25f};
  action.bellows_clip = "1d-blacksmith_axe_heating";
  action.bellows_flags = 258;
  const std::string field = station_action_field(action);
  KCD2S_CHECK(field.find(" B1.2500 P1d-blacksmith_axe_heating,258 HR ") != std::string::npos);
  StationAction parsed;
  KCD2S_CHECK(parse_station_action_field(field, parsed) && parsed == action);
  // A plain clip travels by its id.
  action.bellows_clip = "#74";
  KCD2S_CHECK(parse_station_action_field(station_action_field(action), parsed) && parsed == action);
  action.bellows_clip = "#7a";
  KCD2S_CHECK(station_action_field(action).empty());
  action.bellows_clip = "#";
  KCD2S_CHECK(station_action_field(action).empty());
  action.bellows_clip = "1d-blacksmith_axe_heating";
  // Flags without a clip, and a name the field cannot carry, are not written.
  action.bellows_clip.clear();
  KCD2S_CHECK(station_action_field(action).empty());
  action.bellows_flags = 0;
  action.bellows_clip = "two words";
  KCD2S_CHECK(station_action_field(action).empty());
  action.bellows_clip = std::string(kStationClipMax + 1, 'a');
  KCD2S_CHECK(station_action_field(action).empty());
  // A clip word without its flags, or with a name the field does not write, does not parse.
  const std::string base = station_action_field(smithing());
  const std::size_t props = base.find(" HR ");
  KCD2S_CHECK(!parse_station_action_field(
      base.substr(0, props) + " Pblacksmith_axe_heating" + base.substr(props), parsed));
  KCD2S_CHECK(
      !parse_station_action_field(base.substr(0, props) + " P,0" + base.substr(props), parsed));
  KCD2S_CHECK(!parse_station_action_field(
      base.substr(0, props) + " Pblacksmith_axe_heating," + base.substr(props), parsed));
  KCD2S_CHECK(
      !parse_station_action_field(base.substr(0, props) + " Pa.b,0" + base.substr(props), parsed));
}

KCD2S_TEST(station_action, alchemy_entries) {
  StationAction action;
  action.kind = kStationKindAlchemy;
  action.station = {2299.5f, 2016.375f, 106.375f};
  StationEntry pestle;
  pestle.key = "AlchemyItem_pestle";
  pestle.placed = true;
  pestle.position = {0.25f, -0.5f, 0.875f};
  pestle.rotation = {0.0f, 0.0f, 0.7071f, 0.7071f};
  StationEntry fire;
  fire.key = "particle_fireplace_pot";
  fire.burns = 1;
  fire.spawning = true;
  fire.spawn = {1.0f, 0.75f};
  fire.sounds = {{0x1a2b3c4du, 3, 2}, {0x00000007u, 1, 0}};
  StationEntry light;
  light.key = "light_fireplace_pot";
  light.lit = true;
  light.colour = {2.5f, 1.25f, 0.5f};
  StationEntry herbs;
  herbs.key = "#0";
  herbs.hidden = 0;
  herbs.model = "objects/herbs/herbs_mortar.cgf";
  StationEntry flask;
  flask.key = "AlchemyItem_potion_flask";
  flask.hidden = 1;
  action.entries = {pestle, fire, light, herbs, flask};
  const std::string field = station_action_field(action);
  KCD2S_CHECK(field.find(" @particle_fireplace_pot b1 s1.0000,0.7500 a1a2b3c4d.3.2,00000007.1.0 "
                         "@light_fireplace_pot "
                         "l2.5000,1.2500,0.5000 @#0 v "
                         "mobjects/herbs/herbs_mortar.cgf @AlchemyItem_potion_flask h") !=
              std::string::npos);
  StationAction parsed;
  KCD2S_CHECK(parse_station_action_field(field, parsed) && parsed == action);
  KCD2S_CHECK(!parse_station_action_field(field + " x", parsed));
  KCD2S_CHECK(!parse_station_action_field(field + " @pot a1.2", parsed));
  KCD2S_CHECK(!parse_station_action_field(field + " @pot a1.2.3,", parsed));
  KCD2S_CHECK(!parse_station_action_field(field + " @pot a1.2.300", parsed));
  KCD2S_CHECK(parse_station_action_field(field + " @pot aff.2.3", parsed));
  KCD2S_CHECK(parsed.entries.back().sounds.size() == 1 &&
              parsed.entries.back().sounds[0].trigger == 0xffu);
  KCD2S_CHECK(!parse_station_action_field(field + " @AlchemyItem_pestle", parsed));  // twice
  action.entries[0].position = {20.0f, 0.0f, 0.0f};  // beyond the room
  KCD2S_CHECK(station_action_field(action).empty());
}

KCD2S_TEST(station_action, alchemy_entry_clips) {
  StationAction action;
  action.kind = kStationKindAlchemy;
  action.station = {2299.5f, 2016.375f, 106.375f};
  StationEntry book;
  book.key = "recipesbook_alchemy000045";
  book.hidden = 0;
  book.clip = "#2";
  book.clip_flags = 260;
  StationEntry bellows;
  bellows.key = "AlchemyItem_bellows";
  bellows.clip = "alchemy_bellows_pump";
  action.entries = {book, bellows};
  const std::string field = station_action_field(action);
  KCD2S_CHECK(field.find(" @recipesbook_alchemy000045 v c#2,260 @AlchemyItem_bellows "
                         "calchemy_bellows_pump,0") != std::string::npos);
  StationAction parsed;
  KCD2S_CHECK(parse_station_action_field(field, parsed) && parsed == action);
  KCD2S_CHECK(!parse_station_action_field(field + " c#3,0", parsed));  // twice
  KCD2S_CHECK(!parse_station_action_field(field + " @pot c#3", parsed));
  KCD2S_CHECK(!parse_station_action_field(field + " @pot c,4", parsed));
  action.entries[1].clip.clear();
  action.entries[1].clip_flags = 4;  // flags without a clip
  KCD2S_CHECK(station_action_field(action).empty());
}

KCD2S_TEST(station_action, alchemy_entry_in_hand) {
  StationAction action;
  action.kind = kStationKindAlchemy;
  action.station = {2299.5f, 2016.375f, 106.375f};
  StationEntry pestle;
  pestle.key = "AlchemyItem_pestle";
  pestle.placed = true;
  pestle.position = {0.125f, -0.0625f, 0.0f};
  pestle.hand = 'R';
  action.entries = {pestle};
  const std::string field = station_action_field(action);
  KCD2S_CHECK(field.find(" rR") != std::string::npos);
  StationAction parsed;
  KCD2S_CHECK(parse_station_action_field(field, parsed) && parsed == action);
  KCD2S_CHECK(!parse_station_action_field(field + " rL", parsed));         // twice
  KCD2S_CHECK(!parse_station_action_field(field + " @flask rL", parsed));  // without a place
  action.entries[0].hand = 'X';
  KCD2S_CHECK(station_action_field(action).empty());
}

// A grindstone: the wheel's clip, its sounds and audio parameters, the sparks.
KCD2S_TEST(station_action, grindstone) {
  StationAction action;
  action.kind = kStationKindGrind;
  action.station = {1997.662f, 2501.094f, 115.2578f};
  action.bellows_clip = "grindstone_spinning_sword";
  action.bellows_flags = 2;
  action.turning = true;
  action.wheel_speed = 1.25f;
  action.layer_scales = {StationLayerScale{0, 1.25f}, StationLayerScale{3, 0.5f}};
  action.body_clip = "#15974";
  StationEntry wheel;
  wheel.key = kStationGrindWheel;
  wheel.sounds = {StationSound{0x1a2b3c4d, 1, 0}};
  wheel.parameters = {StationParameter{0x0badf00d, 0.75f}, StationParameter{0x12, -1.5f}};
  StationEntry sparks;
  sparks.key = kStationGrindSparks;
  sparks.spawning = true;
  sparks.spawn = {120.0f, 1.0f};
  action.entries = {wheel, sparks};
  const std::string field = station_action_field(action);
  KCD2S_CHECK(
      field ==
      "grind S1997.6620,2501.0940,115.2578 F0 G00 E0 K0 Q0 A000000000000000000 "
      "Pgrindstone_spinning_sword,2 V1.2500 Y0.1.2500,3.0.5000 D#15974 @wheel a1a2b3c4d.1.0 "
      "u0badf00d.0.7500,00000012.-1.5000 @sparks s120.0000,1.0000");
  StationAction parsed;
  KCD2S_CHECK(parse_station_action_field(field, parsed) && parsed == action);
  const std::string wheel_only =
      "grind S1,2,3 F0 G00 E0 K0 Q0 A000000000000000000 @wheel u12.1.0000";
  KCD2S_CHECK(parse_station_action_field(wheel_only, parsed));
  KCD2S_CHECK(!parse_station_action_field(wheel_only + " u13.1", parsed));  // twice
  KCD2S_CHECK(!parse_station_action_field(
      "grind S1,2,3 F0 G00 E0 K0 Q0 A000000000000000000 @wheel u0.5", parsed));  // no id
  action.layer_scales[1].layer = 0;                                              // a layer twice
  KCD2S_CHECK(station_action_field(action).empty());
  action.layer_scales[1].layer = 16;  // no such layer
  KCD2S_CHECK(station_action_field(action).empty());
  action.layer_scales.pop_back();
  action.entries[0].parameters[0].id = 0;
  KCD2S_CHECK(station_action_field(action).empty());
}

KCD2S_TEST(station_action, workpiece_models) {
  KCD2S_CHECK(std::string(station_workpiece_model("horseshoe")).find("horseshoe.cdf") !=
              std::string::npos);
  KCD2S_CHECK(station_workpiece_model("axe") != nullptr);
  KCD2S_CHECK(station_workpiece_model("sword") != nullptr);
  KCD2S_CHECK(station_workpiece_model("longsword") != nullptr);
  KCD2S_CHECK(station_workpiece_model("spoon") == nullptr);
}
