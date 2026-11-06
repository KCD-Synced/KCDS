#include "kcd2s/integration/progression.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// A player's progression: PROGRESSION and the perk GUID text.
namespace {

// "Pacifist" in rpg/perk__kcd2.xml: 2b9f3c56-d5f4-49b0-a259-8a89a67c8f0d, as the game stores it.
constexpr PerkId kPacifist = {0x56, 0x3c, 0x9f, 0x2b, 0xf4, 0xd5, 0xb0, 0x49,
                              0xa2, 0x59, 0x8a, 0x89, 0xa6, 0x7c, 0x8f, 0x0d};

Progression sample() {
  Progression progression;
  progression.tracks[7] = {19, 50 * kXpUnits, 7};  // survival
  progression.tracks[15] = {5, 3 * kXpUnits + 64, 0};
  progression.main_points = 2;
  PerkId other{};
  other[0] = 1;
  progression.perks = {kPacifist, other};
  normalize_perks(progression.perks);
  return progression;
}

}  // namespace

KCD2S_TEST(progression, perk_id_text) {
  KCD2S_CHECK(perk_id_text(kPacifist) == "2b9f3c56-d5f4-49b0-a259-8a89a67c8f0d");
  KCD2S_CHECK(perk_id_text(PerkId{}) == "00000000-0000-0000-0000-000000000000");
  PerkId parsed{};
  KCD2S_CHECK(parse_perk_id("2B9F3C56-D5F4-49B0-A259-8A89A67C8F0D", parsed) && parsed == kPacifist);
  for (const char* bad :
       {"", "2b9f3c56d5f449b0a2598a89a67c8f0d", "2b9f3c56-d5f4-49b0-a259-8a89a67c8f0g",
        "2b9f3c56-d5f4-49b0-a259-8a89a67c8f0d0"}) {
    KCD2S_CHECK(!parse_perk_id(bad, parsed));
  }
}

KCD2S_TEST(progression, round_trip) {
  const Progression progression = sample();
  Progression decoded;
  KCD2S_CHECK(decode_progression(encode_progression(progression), decoded));
  KCD2S_CHECK(decoded == progression);
  KCD2S_CHECK(decoded.tracks[7].level == 19 && decoded.tracks[7].points == 7);
  KCD2S_CHECK(decoded.tracks[15].xp == 3 * kXpUnits + 64);
  // No perks at all is a snapshot too.
  Progression empty;
  KCD2S_CHECK(decode_progression(encode_progression(empty), decoded) && decoded == empty);
}

KCD2S_TEST(progression, malformed_refused) {
  Progression too_high = sample();
  too_high.tracks[0].level = kLevelCap + 1;
  KCD2S_CHECK(encode_progression(too_high).empty());
  Progression too_many;
  too_many.perks.resize(kMaxPerks + 1);
  KCD2S_CHECK(encode_progression(too_many).empty());

  const auto good = encode_progression(sample());
  Progression decoded = sample();
  decoded.main_points = 99;
  auto cut = good;
  cut.pop_back();
  KCD2S_CHECK(!decode_progression(cut, decoded));
  auto longer = good;
  longer.push_back(0);
  KCD2S_CHECK(!decode_progression(longer, decoded));
  auto level = good;
  level[1] = kLevelCap + 1;
  KCD2S_CHECK(!decode_progression(level, decoded));
  // The perks out of order, or the same perk twice.
  auto swapped = good;
  const std::size_t perks = good.size() - 32;
  std::swap_ranges(swapped.begin() + std::ptrdiff_t(perks),
                   swapped.begin() + std::ptrdiff_t(perks + 16),
                   swapped.begin() + std::ptrdiff_t(perks + 16));
  KCD2S_CHECK(!decode_progression(swapped, decoded));
  auto twice = good;
  std::copy(twice.begin() + std::ptrdiff_t(perks), twice.begin() + std::ptrdiff_t(perks + 16),
            twice.begin() + std::ptrdiff_t(perks + 16));
  KCD2S_CHECK(!decode_progression(twice, decoded));
  // A refused message leaves the snapshot alone.
  KCD2S_CHECK(decoded.main_points == 99);
}

KCD2S_TEST(progression, orders_round_trip) {
  using Kind = ProgressionOrder::Kind;
  std::vector<ProgressionOrder> orders(7);
  orders[0].kind = Kind::AddXp;
  orders[0].track = 8;
  orders[0].xp = 125 * kXpUnits + 32;
  orders[0].multipliers = true;
  orders[1].kind = Kind::SetLevel;
  orders[1].track = 19;
  orders[1].level = kLevelCap;
  orders[1].notify = false;
  orders[2].kind = Kind::AddPerk;
  orders[2].perk = kPacifist;
  orders[3].kind = Kind::RemovePerk;
  orders[3].perk = kPacifist;
  orders[4].kind = Kind::AddPoints;
  orders[4].track = kMainBank;
  orders[4].count = 3;
  orders[5].kind = Kind::Respec;
  orders[6].kind = Kind::Restore;
  orders[6].snapshot = sample();
  for (const auto& order : orders) {
    ProgressionOrder decoded;
    KCD2S_CHECK(decode_progression_order(encode_progression_order(order), decoded));
    KCD2S_CHECK(decoded == order);
  }
  // Quiet orders ({ notify: false }) keep their flag: XP, a level and a restore.
  for (const std::size_t index : {std::size_t{0}, std::size_t{6}}) {
    ProgressionOrder quiet = orders[index];
    quiet.notify = false;
    ProgressionOrder decoded;
    KCD2S_CHECK(decode_progression_order(encode_progression_order(quiet), decoded));
    KCD2S_CHECK(decoded == quiet && !decoded.notify);
  }
}

// The exact orders: setTrack with 0 XP too, and a restore with exact.
KCD2S_TEST(progression, exact_orders_round_trip) {
  using Kind = ProgressionOrder::Kind;
  ProgressionOrder track;
  track.kind = Kind::SetTrack;
  track.track = 16;
  track.level = 4;
  track.xp = 17 * kXpUnits + 5;
  track.notify = true;
  ProgressionOrder decoded;
  KCD2S_CHECK(decode_progression_order(encode_progression_order(track), decoded));
  KCD2S_CHECK(decoded == track);
  track.level = 0;
  track.xp = 0;
  KCD2S_CHECK(decode_progression_order(encode_progression_order(track), decoded));
  KCD2S_CHECK(decoded == track);
  track.level = kLevelCap + 1;
  KCD2S_CHECK(encode_progression_order(track).empty());
  track.level = 1;
  track.xp = kMaxOrderXp + 1;
  KCD2S_CHECK(encode_progression_order(track).empty());
  track.xp = 0;
  track.track = std::uint8_t(kTracks.size());
  KCD2S_CHECK(encode_progression_order(track).empty());
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 12, 0, 1, 0, 0, 0}, decoded));
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 12, 20, 1, 0, 0, 0, 0}, decoded));

  ProgressionOrder restore;
  restore.kind = Kind::Restore;
  restore.snapshot = sample();
  restore.exact = true;
  restore.notify = false;
  KCD2S_CHECK(decode_progression_order(encode_progression_order(restore), decoded));
  KCD2S_CHECK(decoded == restore && decoded.exact && !decoded.notify);
  restore.exact = false;
  KCD2S_CHECK(decode_progression_order(encode_progression_order(restore), decoded));
  KCD2S_CHECK(!decoded.exact);
}

KCD2S_TEST(progression, orders_refused) {
  using Kind = ProgressionOrder::Kind;
  ProgressionOrder xp;
  xp.kind = Kind::AddXp;
  xp.track = 0;
  xp.xp = 0;
  KCD2S_CHECK(encode_progression_order(xp).empty());
  xp.xp = kMaxOrderXp + 1;
  KCD2S_CHECK(encode_progression_order(xp).empty());
  xp.xp = kXpUnits;
  xp.track = std::uint8_t(kTracks.size());
  KCD2S_CHECK(encode_progression_order(xp).empty());
  ProgressionOrder level;
  level.kind = Kind::SetLevel;
  level.level = kLevelCap + 1;
  KCD2S_CHECK(encode_progression_order(level).empty());
  ProgressionOrder points;
  points.kind = Kind::AddPoints;
  points.track = 20;
  points.count = 1;
  KCD2S_CHECK(encode_progression_order(points).empty());
  points.track = kMainBank;
  points.count = 0;
  KCD2S_CHECK(encode_progression_order(points).empty());
  ProgressionOrder decoded;
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 9}, decoded));
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 6, 0}, decoded));
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 2, 0, kLevelCap + 1, 1}, decoded));
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 2, 0, 1, 2}, decoded));
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 1, 0, 0, 0, 0, 0, 0, 1}, decoded));
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 1, 0, 128, 0, 0, 0, 2, 1}, decoded));
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 1, 0, 128, 0, 0, 0, 1, 2}, decoded));
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 7, 2}, decoded));
}

KCD2S_TEST(progression, native_xp_order_and_xp_gaining) {
  ProgressionOrder off;
  off.kind = ProgressionOrder::Kind::NativeXp;
  off.level = 0;
  ProgressionOrder decoded;
  decoded.level = 1;
  KCD2S_CHECK(decode_progression_order(encode_progression_order(off), decoded));
  KCD2S_CHECK(decoded.kind == ProgressionOrder::Kind::NativeXp && decoded.level == 0);
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 8, 2}, decoded));

  XpGain gain;
  gain.track = 8;
  gain.xp = 12 * kXpUnits + 5;
  gain.multipliers = true;
  gain.source = "LockpickingResult";
  XpGain read;
  KCD2S_CHECK(decode_xp_gaining(encode_xp_gaining(gain), read) && read == gain);
  gain.source = "";
  KCD2S_CHECK(decode_xp_gaining(encode_xp_gaining(gain), read) && read == gain);
  // What cannot travel: an unknown track, no XP, too much, a source with other characters.
  XpGain bad = gain;
  bad.track = std::uint8_t(kTracks.size());
  KCD2S_CHECK(encode_xp_gaining(bad).empty());
  bad = gain;
  bad.xp = 0;
  KCD2S_CHECK(encode_xp_gaining(bad).empty());
  bad = gain;
  bad.source = "Attack; drop";
  KCD2S_CHECK(encode_xp_gaining(bad).empty());
  bad.source = std::string(kMaxXpSourceBytes + 1, 'a');
  KCD2S_CHECK(encode_xp_gaining(bad).empty());
  auto message = encode_xp_gaining(gain);
  message.push_back('x');
  KCD2S_CHECK(!decode_xp_gaining(message, read));
}

KCD2S_TEST(progression, perk_learning_and_block_list) {
  PerkId learnt{};
  KCD2S_CHECK(decode_perk_learning(encode_perk_learning(kPacifist), learnt) && learnt == kPacifist);
  KCD2S_CHECK(!decode_perk_learning({kPerkLearningTag, 1, 2}, learnt));
  ProgressionOrder learn;
  learn.kind = ProgressionOrder::Kind::LearnPerk;
  learn.perk = kPacifist;
  ProgressionOrder decoded;
  KCD2S_CHECK(decode_progression_order(encode_progression_order(learn), decoded) &&
              decoded == learn);
  ProgressionOrder block;
  block.kind = ProgressionOrder::Kind::BlockPerks;
  PerkId other{};
  other[3] = 7;
  block.perks = {kPacifist, other};
  KCD2S_CHECK(decode_progression_order(encode_progression_order(block), decoded) &&
              decoded == block);
  // An empty list lifts every block.
  block.perks.clear();
  KCD2S_CHECK(decode_progression_order(encode_progression_order(block), decoded) &&
              decoded.perks.empty());
  KCD2S_CHECK(!decode_progression_order({kProgressionOrderTag, 10, 1, 0}, decoded));
  ProgressionOrder refuse;
  refuse.kind = ProgressionOrder::Kind::RefusePerk;
  refuse.perk = kPacifist;
  refuse.refusal = PerkRefusal::Blocked;
  KCD2S_CHECK(decode_progression_order(encode_progression_order(refuse), decoded) &&
              decoded == refuse);
  refuse.refusal = PerkRefusal(kPerkRefusalCount);
  KCD2S_CHECK(encode_progression_order(refuse).empty());
}
