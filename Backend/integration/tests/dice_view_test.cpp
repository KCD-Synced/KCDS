#include "kcd2s/integration/dice_view.hpp"

#include <limits>

#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// DICE_VIEW and DICE_WATCH.

KCD2S_TEST(dice_view, round_trip) {
  DiceThing die;
  die.key = 0x9ac1dfdabb420f49ull;
  die.position = {2329.3f, 2080.7f, 111.2f};
  die.rotation = {0.0f, 0.7071f, 0.0f, 0.7071f};
  die.model = "objects/manmade/task_specific_props/entertainment/games/dice/die_g.cgf";
  die.scale = 1.25f;
  die.material = "objects/manmade/task_specific_props/entertainment/games/dice/dice_normal";
  DiceThing cup = die;
  cup.key = 7;
  cup.shown = false;
  cup.model.clear();
  cup.material.clear();
  cup.scale = 1.0f;
  DiceView view{12, {die, cup}};
  DiceView decoded_view;
  KCD2S_CHECK(decode_dice_view(encode_dice_view(view), decoded_view) && decoded_view == view);
  DiceWatch watch{12, false, {die}};
  DiceWatch decoded_watch;
  KCD2S_CHECK(decode_dice_watch(encode_dice_watch(watch), decoded_watch) && decoded_watch == watch);
  DiceWatch ended{12, true, {}};
  KCD2S_CHECK(decode_dice_watch(encode_dice_watch(ended), decoded_watch) && decoded_watch == ended);
}

KCD2S_TEST(dice_view, carries_the_throwers_board_camera) {
  DiceThing camera;
  camera.key = kDiceViewCamera;
  camera.position = {2003.1f, 2531.4f, 120.6f};
  camera.rotation = {-0.21f, 0.05f, 0.95f, 0.22f};
  camera.shown = false;
  DiceView view{3, {camera}};
  DiceView decoded;
  KCD2S_CHECK(decode_dice_view(encode_dice_view(view), decoded) && decoded == view);
  KCD2S_CHECK(decoded.things.front().key == kDiceViewCamera);
}

KCD2S_TEST(dice_view, refuses_what_it_cannot_carry) {
  DiceThing die;
  die.key = 1;
  DiceView view{1, {}};
  view.things.assign(kMaxDiceViewThings + 1, die);
  KCD2S_CHECK(encode_dice_view(view).empty());
  view.things = {die};
  view.things[0].key = 0;
  KCD2S_CHECK(encode_dice_view(view).empty());
  view.things[0].key = 1;
  view.things[0].position[1] = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(encode_dice_view(view).empty());
  view.things[0].position[1] = 0.0f;
  view.things[0].rotation[0] = 2.0f;
  KCD2S_CHECK(encode_dice_view(view).empty());
  view.things[0].rotation[0] = 0.0f;
  view.things[0].scale = 0.0f;
  KCD2S_CHECK(encode_dice_view(view).empty());
  view.things[0].scale = 1.0f;
  view.things[0].model = "objects/die g.cgf";
  KCD2S_CHECK(encode_dice_view(view).empty());
  view.things[0].model.assign(kMaxDiceModel + 1, 'a');
  KCD2S_CHECK(encode_dice_view(view).empty());
  view.things[0].model = "objects/die_g.cgf";
  view.things[0].material = "dice normal";
  KCD2S_CHECK(encode_dice_view(view).empty());
  view.things[0].material.clear();
  auto bytes = encode_dice_view(view);
  DiceView decoded;
  bytes.push_back(0);
  KCD2S_CHECK(!decode_dice_view(bytes, decoded));
  KCD2S_CHECK(!decode_dice_view({}, decoded));
}

KCD2S_TEST(dice_view, tables_carry_whether_a_table_use_is_answered) {
  DiceTables decoded;
  KCD2S_CHECK(decode_dice_tables(encode_dice_tables(DiceTables{true}), decoded) && decoded.offered);
  KCD2S_CHECK(decode_dice_tables(encode_dice_tables(DiceTables{false}), decoded) &&
              !decoded.offered);
  auto bytes = encode_dice_tables(DiceTables{true});
  bytes[1] = 2;
  KCD2S_CHECK(!decode_dice_tables(bytes, decoded));
  bytes[1] = 1;
  bytes.push_back(0);
  KCD2S_CHECK(!decode_dice_tables(bytes, decoded));
  KCD2S_CHECK(!decode_dice_tables({kDiceTablesTag}, decoded));
}
