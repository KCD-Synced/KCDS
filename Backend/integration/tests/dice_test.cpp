#include "kcd2s/integration/dice.hpp"

#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// DICE_STATE and DICE_MOVE.

KCD2S_TEST(dice, states_round_trip) {
  DiceState start;
  start.kind = DiceState::Kind::Start;
  start.match = 70000;
  start.opponent = 12;
  start.behavior = 0xedcf5168b3960b3aull;
  start.seat = 1;
  start.first = true;
  start.target = 2000;
  start.own_name = "Henry";
  start.opponent_name = std::string(kMaxDiceNameBytes, 'h');
  DiceState decoded;
  KCD2S_CHECK(decode_dice_state(encode_dice_state(start), decoded) && decoded == start);
  // Dice of the game's kinds: six meshes for a player, none for the game's plain dice.
  start.own_dice.assign(6,
                        "objects/manmade/task_specific_props/entertainment/games/dice/die_o.cgf");
  start.own_sides.assign(6, {6, 1, 2, 3, 4, 5});
  KCD2S_CHECK(decode_dice_state(encode_dice_state(start), decoded) && decoded == start);
  // Sides only with meshes, each a value 0 to 6.
  start.own_sides.assign(5, {0, 1, 2, 3, 4, 5});
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.own_sides.assign(6, {7, 1, 2, 3, 4, 5});
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.own_sides.assign(6, {6, 1, 2, 3, 4, 5});
  // Badges: the item's class, an effect of the game's 13 and a tier of its 3; none without.
  KCD2S_CHECK(parse_guid("b26e6c86-2970-4106-a25b-9e4ffa181979", start.own_badge.item));
  start.own_badge.effect = 4;
  start.own_badge.tier = 2;
  KCD2S_CHECK(decode_dice_state(encode_dice_state(start), decoded) && decoded == start);
  start.opponent_badge = start.own_badge;
  start.opponent_badge.effect = 12;
  start.opponent_badge.tier = 0;
  KCD2S_CHECK(decode_dice_state(encode_dice_state(start), decoded) && decoded == start);
  start.opponent_badge.effect = kDiceBadgeEffects;
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.opponent_badge.effect = 12;
  start.opponent_badge.tier = kDiceBadgeTiers;
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.opponent_badge = DiceBadge{};
  start.opponent_badge.tier = 1;
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.opponent_badge = DiceBadge{};
  KCD2S_CHECK(decode_dice_state(encode_dice_state(start), decoded) && decoded == start);
  DiceState roll;
  roll.kind = DiceState::Kind::Roll;
  roll.match = 3;
  roll.mine = true;
  roll.faces = {5, 1, 1, 2, 2, 3};
  roll.rolled = 0x3E;
  KCD2S_CHECK(decode_dice_state(encode_dice_state(roll), decoded) && decoded == roll);
  roll.bust = true;
  roll.mine = false;
  KCD2S_CHECK(decode_dice_state(encode_dice_state(roll), decoded) && decoded == roll);
  DiceState move;
  move.kind = DiceState::Kind::Move;
  move.match = 3;
  move.move = DiceState::Move::Pass;
  move.held = 0x06;
  KCD2S_CHECK(decode_dice_state(encode_dice_state(move), decoded) && decoded == move);
  // The dice selected so far: none is a selection too.
  move.move = DiceState::Move::Select;
  move.held = 0;
  KCD2S_CHECK(decode_dice_state(encode_dice_state(move), decoded) && decoded == move);
  DiceState end;
  end.kind = DiceState::Kind::End;
  end.match = 3;
  end.reason = DiceEnd::TimedOut;
  end.result = DiceResult::Won;
  KCD2S_CHECK(decode_dice_state(encode_dice_state(end), decoded) && decoded == end);
  DiceState names;
  names.kind = DiceState::Kind::Names;
  names.match = 3;
  names.own_name = "Hans";
  KCD2S_CHECK(decode_dice_state(encode_dice_state(names), decoded) && decoded == names);
  // Sitting down at a table to wait for a match: no match yet, one of its two seats.
  DiceState sit;
  sit.kind = DiceState::Kind::Sit;
  sit.behavior = 0x0123456789abcdefull;
  sit.seat = 1;
  KCD2S_CHECK(decode_dice_state(encode_dice_state(sit), decoded) && decoded == sit);
  sit.seat = 2;
  KCD2S_CHECK(encode_dice_state(sit).empty());
  sit.seat = 0;
  sit.match = 3;
  KCD2S_CHECK(encode_dice_state(sit).empty());
  sit.match = 0;
  sit.behavior = 0;
  KCD2S_CHECK(encode_dice_state(sit).empty());
}

KCD2S_TEST(dice, refuses_states_it_cannot_carry) {
  DiceState start;
  start.kind = DiceState::Kind::Start;
  start.target = 0;
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.target = kMaxDiceTarget + 1;
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.target = 100;
  start.seat = 2;
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.seat = 0;
  start.own_name = "a\nb";
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.own_name = std::string(kMaxDiceNameBytes + 1, 'a');
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.own_name.clear();
  start.own_dice.assign(5, "objects/die_o.cgf");
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.own_dice.assign(6, "objects/die o.cgf");
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.own_dice.assign(6, std::string(kMaxDiceMeshBytes + 1, 'a'));
  KCD2S_CHECK(encode_dice_state(start).empty());
  start.own_dice.clear();
  DiceState roll;
  roll.kind = DiceState::Kind::Roll;
  roll.rolled = 0x01;
  roll.faces = {8, 0, 0, 0, 0, 0};
  KCD2S_CHECK(encode_dice_state(roll).empty());
  // The joker side of a joker die travels.
  roll.faces = {kDiceJoker, 0, 0, 0, 0, 0};
  DiceState decoded_joker;
  KCD2S_CHECK(decode_dice_state(encode_dice_state(roll), decoded_joker) && decoded_joker == roll);
  // A Might badge's seventh die travels; an eighth does not.
  roll.faces = {6, 0, 0, 0, 0, 0, 0};
  roll.rolled = 0x40;
  KCD2S_CHECK(encode_dice_state(roll).empty());
  roll.faces = {6, 0, 0, 0, 0, 0, 3};
  KCD2S_CHECK(decode_dice_state(encode_dice_state(roll), decoded_joker) && decoded_joker == roll);
  roll.rolled = 0x80;
  KCD2S_CHECK(encode_dice_state(roll).empty());
  DiceState move;
  move.kind = DiceState::Kind::Move;
  move.held = 0;
  KCD2S_CHECK(encode_dice_state(move).empty());
  // Badge moves: the dice a badge changed (none for a Might badge), a badge pass with dice, a
  // Resurrection choice of three.
  move.move = DiceState::Move::Badge;
  KCD2S_CHECK(!encode_dice_state(move).empty());
  move.held = 0x41;
  KCD2S_CHECK(!encode_dice_state(move).empty());
  move.held = 0x80;
  KCD2S_CHECK(encode_dice_state(move).empty());
  move.move = DiceState::Move::BadgePass;
  move.held = 0;
  KCD2S_CHECK(encode_dice_state(move).empty());
  move.held = 0x60;
  KCD2S_CHECK(!encode_dice_state(move).empty());
  move.move = DiceState::Move::BustChoice;
  move.held = 2;
  KCD2S_CHECK(!encode_dice_state(move).empty());
  move.held = kDiceBustChoices;
  KCD2S_CHECK(encode_dice_state(move).empty());
  DiceState decoded;
  KCD2S_CHECK(!decode_dice_state({}, decoded));
  KCD2S_CHECK(!decode_dice_state({kDiceStateTag, 9, 1}, decoded));
  auto bytes = encode_dice_state(DiceState{DiceState::Kind::End});
  KCD2S_CHECK(decode_dice_state(bytes, decoded));
  bytes.push_back(0);
  KCD2S_CHECK(!decode_dice_state(bytes, decoded));
}

KCD2S_TEST(dice, moves_round_trip) {
  DiceMove move{9, DiceMove::Kind::Continue, 0x21};
  DiceMove decoded;
  KCD2S_CHECK(decode_dice_move(encode_dice_move(move), decoded) && decoded == move);
  move.kind = DiceMove::Kind::GaveUp;
  move.held = 0;
  KCD2S_CHECK(decode_dice_move(encode_dice_move(move), decoded) && decoded == move);
  move.held = 0x80;
  KCD2S_CHECK(encode_dice_move(move).empty());
  // The game throws, and the dice selected so far.
  move.held = 0;
  move.kind = DiceMove::Kind::Throw;
  KCD2S_CHECK(decode_dice_move(encode_dice_move(move), decoded) && decoded == move);
  move.kind = DiceMove::Kind::Select;
  move.held = 0x05;
  KCD2S_CHECK(decode_dice_move(encode_dice_move(move), decoded) && decoded == move);
  // A dice table used, by a player who plays no match.
  move.match = 0;
  move.kind = DiceMove::Kind::TableUse;
  move.held = 0;
  KCD2S_CHECK(decode_dice_move(encode_dice_move(move), decoded) && decoded == move);
  // Badge moves: a badge pass, a badge on the seventh die and another, a Resurrection choice.
  move.match = 9;
  move.kind = DiceMove::Kind::BadgePass;
  move.held = 0x03;
  KCD2S_CHECK(decode_dice_move(encode_dice_move(move), decoded) && decoded == move);
  move.kind = DiceMove::Kind::Badge;
  move.held = 0x41;
  KCD2S_CHECK(decode_dice_move(encode_dice_move(move), decoded) && decoded == move);
  move.kind = DiceMove::Kind::BustChoice;
  move.held = 1;
  KCD2S_CHECK(decode_dice_move(encode_dice_move(move), decoded) && decoded == move);
  KCD2S_CHECK(!decode_dice_move({kDiceMoveTag, 1, 11, 0}, decoded));
  KCD2S_CHECK(!decode_dice_move({kDiceStateTag, 1, 1, 0}, decoded));
}
