#include "kcd2s/integration/buffs.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// The status effects on a player's body: BUFFS, BUFF_ORDER and BUFF_BLOCKED.
namespace {

Guid marigold() {
  Guid id{};
  parse_guid("8503216a-a34c-49f0-aefa-54d4502046f9", id);  // potion_marigold_decoction
  return id;
}

}  // namespace

KCD2S_TEST(buffs, guid_text) {
  KCD2S_CHECK(guid_text(marigold()) == "8503216a-a34c-49f0-aefa-54d4502046f9");
  Guid parsed{};
  KCD2S_CHECK(!parse_guid("8503216a-a34c-49f0-aefa-54d4502046fz", parsed));
}

KCD2S_TEST(buffs, report_round_trip) {
  std::vector<BuffEntry> buffs = {{marigold(), 7, false}, {Guid{}, 0x1234567890ull, true}};
  std::vector<BuffEntry> decoded;
  const auto message = encode_buffs(buffs);
  KCD2S_CHECK(decode_buffs(message, decoded) && decoded == buffs);
  // Small instance ids take one byte each.
  KCD2S_CHECK(encode_buffs({{marigold(), 7, false}}).size() == 1 + 1 + 16 + 1 + 1);
  KCD2S_CHECK(decode_buffs(encode_buffs({}), decoded) && decoded.empty());
  std::vector<BuffEntry> too_many(kMaxBuffs + 1);
  KCD2S_CHECK(encode_buffs(too_many).empty());
  auto truncated = message;
  truncated.pop_back();
  KCD2S_CHECK(!decode_buffs(truncated, decoded));
  auto trailing = message;
  trailing.push_back(0);
  KCD2S_CHECK(!decode_buffs(trailing, decoded));
}

KCD2S_TEST(buffs, orders_round_trip) {
  using Kind = BuffOrder::Kind;
  BuffOrder add;
  add.kind = Kind::Add;
  add.buff = marigold();
  BuffOrder remove;
  remove.kind = Kind::Remove;
  remove.buff = marigold();
  remove.instances = {3, 400000};
  BuffOrder clear;
  clear.kind = Kind::Clear;
  clear.tag = 3;
  BuffOrder claim;
  claim.kind = Kind::Claim;
  claim.classes = {10, 13};
  BuffOrder decoded;
  for (const BuffOrder& order : {add, remove, clear, claim}) {
    KCD2S_CHECK(decode_buff_order(encode_buff_order(order), decoded) && decoded == order);
  }
  // An empty claim hands every class back.
  claim.classes.clear();
  KCD2S_CHECK(decode_buff_order(encode_buff_order(claim), decoded) && decoded.classes.empty());
  claim.classes = {kMaxBuffClasses};
  KCD2S_CHECK(encode_buff_order(claim).empty());
  KCD2S_CHECK(!decode_buff_order({kBuffOrderTag, 9}, decoded));
  KCD2S_CHECK(!decode_buff_order({kBuffOrderTag, 3}, decoded));
  KCD2S_CHECK(!decode_buff_order({kBuffOrderTag, 4, 1, kMaxBuffClasses}, decoded));
}

KCD2S_TEST(buffs, blocked_round_trip) {
  Guid decoded{};
  KCD2S_CHECK(decode_buff_blocked(encode_buff_blocked(marigold()), decoded) &&
              decoded == marigold());
  KCD2S_CHECK(!decode_buff_blocked({kBuffBlockedTag, 1}, decoded));
}
