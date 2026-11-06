#include "kcd2s/integration/horse_buffs.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// The status effects a server put on a horse: the HorseBuffs field.
namespace {

Guid buff(std::uint8_t first) {
  Guid id{};
  id[0] = first;
  id[15] = 0xab;
  return id;
}

}  // namespace

KCD2S_TEST(horse_buffs, round_trip_in_order) {
  const std::vector<Guid> buffs = {buff(3), buff(1)};
  const std::string field = horse_buffs_field(buffs);
  KCD2S_CHECK_EQ(field.size(), std::size_t(74));
  std::vector<Guid> parsed;
  KCD2S_CHECK(parse_horse_buffs_field(field, parsed));
  KCD2S_CHECK(parsed == buffs);
  KCD2S_CHECK(parse_horse_buffs_field("", parsed));
  KCD2S_CHECK(parsed.empty());
  KCD2S_CHECK_EQ(horse_buffs_field({}), std::string{});
}

KCD2S_TEST(horse_buffs, refuses_repeats_too_many_and_bad_text) {
  KCD2S_CHECK_EQ(horse_buffs_field({buff(1), buff(1)}), std::string{});
  std::vector<Guid> many;
  for (std::uint8_t i = 0; i <= kMaxHorseBuffs; ++i) {
    many.push_back(buff(i));
  }
  KCD2S_CHECK_EQ(horse_buffs_field(many), std::string{});
  many.pop_back();
  std::vector<Guid> parsed;
  KCD2S_CHECK(parse_horse_buffs_field(horse_buffs_field(many), parsed));
  KCD2S_CHECK_EQ(parsed.size(), kMaxHorseBuffs);

  const std::string one = guid_text(buff(1)) + ",";
  KCD2S_CHECK(!parse_horse_buffs_field(one + one, parsed));
  KCD2S_CHECK(!parse_horse_buffs_field(guid_text(buff(1)), parsed));
  KCD2S_CHECK(!parse_horse_buffs_field(guid_text(buff(1)) + ";", parsed));
  std::string bad = one;
  bad[0] = 'x';
  KCD2S_CHECK(!parse_horse_buffs_field(bad, parsed));
  // Upper-case digits are the same GUID.
  std::string upper = one;
  for (auto& c : upper) {
    if (c >= 'a' && c <= 'f') {
      c = char(c - 'a' + 'A');
    }
  }
  KCD2S_CHECK(parse_horse_buffs_field(upper, parsed));
  KCD2S_CHECK(parsed.size() == 1 && parsed[0] == buff(1));
}
