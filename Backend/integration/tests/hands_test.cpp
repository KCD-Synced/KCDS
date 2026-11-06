#include "kcd2s/integration/carry.hpp"
#include "kcd2s/integration/hands.hpp"
#include "kcd2s/testkit.hpp"

#include <limits>
using namespace kcd2s::integration;

namespace {

ItemClassId class_id(std::uint8_t first) {
  ItemClassId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = std::uint8_t(first + i);
  }
  return id;
}

}  // namespace

// Hands: the "hands" field carries the item classes in each hand.
KCD2S_TEST(hands, field_round_trip) {
  Hands hands;
  KCD2S_CHECK(hands_field(hands).empty());
  KCD2S_CHECK(parse_hands_field("", hands) && !hands.right && !hands.left);

  hands.right = class_id(0xA0);
  hands.left = class_id(0x01);
  const std::string both = hands_field(hands);
  KCD2S_CHECK(both == "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF|0102030405060708090A0B0C0D0E0F10");
  Hands parsed;
  KCD2S_CHECK(parse_hands_field(both, parsed) && parsed == hands);

  hands.left.reset();
  KCD2S_CHECK(hands_field(hands) == "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF|");
  KCD2S_CHECK(parse_hands_field(hands_field(hands), parsed) && parsed == hands);

  hands.right.reset();
  hands.left = class_id(0x10);
  KCD2S_CHECK(hands_field(hands) == "|101112131415161718191A1B1C1D1E1F");
  KCD2S_CHECK(parse_hands_field(hands_field(hands), parsed) && parsed == hands);
}

KCD2S_TEST(hands, malformed_fields_refused) {
  const char* refused[] = {
      "|",                                                                    // both empty
      "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF",                                     // no bar
      "A0A1A2A3A4A5A6A7A8A9AAABACADAE|",                                      // too short
      "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf|",                                    // lower case
      "G0A1A2A3A4A5A6A7A8A9AAABACADAEAF|",                                    // not hex
      "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF||",                                   // two bars
      "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF00|0102030405060708090A0B0C0D0E0F10",  // too long
  };
  for (const char* field : refused) {
    Hands hands;
    KCD2S_CHECK(!parse_hands_field(field, hands));
  }
}

KCD2S_TEST(hands, lifetime_field_round_trip_and_legacy_identity_is_unknown) {
  Hands hands;
  hands.right = class_id(0xA0);
  hands.left = class_id(0x01);
  hands.right_token = 1;
  hands.left_token = std::numeric_limits<std::uint32_t>::max();
  const auto field = hands_field(hands);
  KCD2S_CHECK(field ==
              "H2:A0A1A2A3A4A5A6A7A8A9AAABACADAEAF:00000001|"
              "0102030405060708090A0B0C0D0E0F10:FFFFFFFF");
  Hands decoded;
  KCD2S_CHECK(parse_hands_field(field, decoded) && decoded == hands);
  const auto previous = hands;
  ++hands.right_token;
  KCD2S_CHECK(!(hands == previous));
  hands.left.reset();
  hands.left_token = 0;
  KCD2S_CHECK(parse_hands_field(hands_field(hands), decoded) && decoded == hands);
  hands.right.reset();
  hands.right_token = 0;
  hands.left = class_id(0x01);
  hands.left_token = 7;
  KCD2S_CHECK(hands_field(hands).starts_with("H2:|"));
  KCD2S_CHECK(parse_hands_field(hands_field(hands), decoded) && decoded == hands);

  KCD2S_CHECK(parse_hands_field("A0A1A2A3A4A5A6A7A8A9AAABACADAEAF|", decoded));
  KCD2S_CHECK(decoded.right && decoded.right_token == 0 && decoded.left_token == 0);
  hands.right = class_id(0xA0);
  // A present hand may have unknown identity while another has a known token.
  KCD2S_CHECK(parse_hands_field(hands_field(hands), decoded) && decoded == hands);
}

KCD2S_TEST(hands, lifetime_fields_reject_malformed_and_conflicting_tokens) {
  const char* refused[] = {
      "H2:|",
      "H2:A0A1A2A3A4A5A6A7A8A9AAABACADAEAF|",
      "H2:A0A1A2A3A4A5A6A7A8A9AAABACADAEAF:1|",
      "H2:A0A1A2A3A4A5A6A7A8A9AAABACADAEAF:0000000001|",
      "H2:A0A1A2A3A4A5A6A7A8A9AAABACADAEAF:0000000a|",
      "H2:A0A1A2A3A4A5A6A7A8A9AAABACADAEAF:0000000G|",
      "H2:A0A1A2A3A4A5A6A7A8A9AAABACADAEAF:00000000|",
      "H2::00000001|",
      "H2:A0A1A2A3A4A5A6A7A8A9AAABACADAEAF:00000001||",
      "H2:A0A1A2A3A4A5A6A7A8A9AAABACADAEAF:00000001|"
      "0102030405060708090A0B0C0D0E0F10:00000001",
      "H3:A0A1A2A3A4A5A6A7A8A9AAABACADAEAF:00000001|",
  };
  for (const char* field : refused) {
    Hands decoded;
    decoded.right = class_id(0x01);
    decoded.right_token = 7;
    KCD2S_CHECK(!parse_hands_field(field, decoded));
    KCD2S_CHECK(!decoded.right && !decoded.left && !decoded.right_token && !decoded.left_token);
  }
  Hands invalid;
  invalid.right_token = 8;
  invalid.left = class_id(0x01);
  KCD2S_CHECK(hands_field(invalid).empty());
}

KCD2S_TEST(hands, owner_tokens_follow_item_lifetime_including_same_class_replacement) {
  const LocalHandItem first{0x20000000000A491, 0x1000, class_id(0xA0)};
  ItemLifetimeTokens tokens;
  LocalHandItems current{first, std::nullopt};
  const auto original = tokens.token_for(first, current);
  KCD2S_CHECK(original && *original != 0);
  // Holstering does not destroy the item; redrawing can retain its token.
  KCD2S_CHECK(tokens.token_for(first, {}) == original);
  KCD2S_CHECK(tokens.token_for(first, current) == original);
  auto replacement = first;
  replacement.uid += 1;
  current[0] = replacement;
  const auto swapped = tokens.token_for(replacement, current);
  KCD2S_CHECK(swapped && swapped != original);
  // Reused native ID with a new allocation or class is a new lifetime too.
  replacement.instance += 1;
  current[0] = replacement;
  const auto allocated = tokens.token_for(replacement, current);
  KCD2S_CHECK(allocated && allocated != swapped);
  replacement.item_class = class_id(0x10);
  current[0] = replacement;
  const auto reclassified = tokens.token_for(replacement, current);
  KCD2S_CHECK(reclassified && reclassified != allocated);
  KCD2S_CHECK(!tokens.token_for(LocalHandItem{}, current));
  replacement.instance = 0;
  KCD2S_CHECK(!tokens.token_for(replacement, current));

  tokens.clear();
  KCD2S_CHECK_EQ(tokens.size(), std::size_t(0));
  const auto after_reset = tokens.token_for(first, {first, std::nullopt});
  KCD2S_CHECK(after_reset && after_reset != original);
}

KCD2S_TEST(hands, owner_cache_evicts_noncurrent_lru_without_reusing_tokens) {
  ItemLifetimeTokens tokens;
  const LocalHandItem anchor{1, 0x1000, class_id(0xA0)};
  LocalHandItems current{anchor, std::nullopt};
  const auto anchor_token = tokens.token_for(anchor, current);
  std::optional<std::uint32_t> second_token;
  LocalHandItem latest;
  std::optional<std::uint32_t> latest_token;
  for (std::uint64_t uid = 2; uid <= 128; ++uid) {
    latest = LocalHandItem{uid, std::uintptr_t(0x1000 + uid), class_id(0xA0)};
    current[1] = latest;
    latest_token = tokens.token_for(latest, current);
    KCD2S_CHECK(latest_token && *latest_token != 0);
    if (uid == 2) {
      second_token = latest_token;
    }
    KCD2S_CHECK(tokens.size() <= ItemLifetimeTokens::kCapacity);
  }
  KCD2S_CHECK_EQ(tokens.size(), ItemLifetimeTokens::kCapacity);
  KCD2S_CHECK(tokens.token_for(anchor, current) == anchor_token);
  KCD2S_CHECK(tokens.token_for(latest, current) == latest_token);
  const LocalHandItem evicted{2, 0x1002, class_id(0xA0)};
  const auto redrawn = tokens.token_for(evicted, {anchor, evicted});
  KCD2S_CHECK(redrawn && redrawn != second_token);
  KCD2S_CHECK_EQ(tokens.size(), ItemLifetimeTokens::kCapacity);
}

KCD2S_TEST(hands, removed_native_items_are_forgotten_and_token_exhaustion_fails_closed) {
  const LocalHandItem item{1, 0x1000, class_id(0xA0)};
  ItemLifetimeTokens tokens;
  const auto first = tokens.token_for(item, {item, std::nullopt});
  tokens.retain_valid([](std::uint64_t) -> std::optional<LocalHandItem> { return std::nullopt; });
  KCD2S_CHECK_EQ(tokens.size(), std::size_t(0));
  const auto returned = tokens.token_for(item, {item, std::nullopt});
  KCD2S_CHECK(returned && returned != first);

  ItemLifetimeTokens exhausted(std::numeric_limits<std::uint32_t>::max());
  const auto last = exhausted.token_for(item, {item, std::nullopt});
  KCD2S_CHECK(last && *last == std::numeric_limits<std::uint32_t>::max());
  const LocalHandItem next{2, 0x2000, item.item_class};
  KCD2S_CHECK(!exhausted.token_for(next, {next, std::nullopt}));
  KCD2S_CHECK(exhausted.token_for(item, {item, std::nullopt}) == last);
  exhausted.clear();
  KCD2S_CHECK(!exhausted.token_for(item, {item, std::nullopt}));
}

KCD2S_TEST(hands, replica_bindings_reject_stale_combat_after_same_class_swap_and_drop) {
  Hands owner;
  owner.right = class_id(0xA0);
  owner.right_token = 41;
  const LocalHandItem item{0x20000000000A491, 0x1000, *owner.right};
  LocalHandItems current{item, std::nullopt};
  HandItemBindings bindings;
  bindings.bind(owner, current);
  KCD2S_CHECK(bindings.item_from(41, current) == item.uid);
  KCD2S_CHECK(bindings.token_for(item.uid, current) == 41);
  KCD2S_CHECK(bindings.item_from(0, current) == 0);
  KCD2S_CHECK(bindings.token_for(0, current) == 0);
  KCD2S_CHECK(!bindings.item_from(99, current));
  KCD2S_CHECK(!bindings.token_for(99, current));
  // A same-class owner replacement can reuse the replica's presentation item,
  // but an old combat token must never alias its new lifetime.
  owner.right_token = 42;
  bindings.bind(owner, current);
  KCD2S_CHECK(!bindings.item_from(41, current));
  KCD2S_CHECK(bindings.item_from(42, current) == item.uid);
  current[0].reset();
  KCD2S_CHECK(!bindings.item_from(42, current));
  KCD2S_CHECK(!bindings.token_for(item.uid, current));
  bindings.bind({}, current);
  current[0] = item;
  KCD2S_CHECK(!bindings.item_from(42, current));
  bindings.clear();
  KCD2S_CHECK(!bindings.token_for(item.uid, current));
}

KCD2S_TEST(hands, replica_binding_requires_matching_current_native_instance_and_class) {
  Hands owner;
  owner.right = class_id(0xA0);
  owner.right_token = 9;
  const LocalHandItem item{4, 0x1000, *owner.right};
  HandItemBindings bindings;
  LocalHandItems current{};
  bindings.bind(owner, current);  // the draw has not reached the hand yet
  KCD2S_CHECK(!bindings.item_from(9, current));
  current[0] = item;
  KCD2S_CHECK(!bindings.item_from(9, current));
  bindings.bind(owner, current);
  KCD2S_CHECK(bindings.item_from(9, current) == item.uid);
  current[0]->instance += 1;
  KCD2S_CHECK(!bindings.item_from(9, current));
  KCD2S_CHECK(!bindings.token_for(item.uid, current));
  current[0] = item;
  current[0]->item_class = class_id(0x10);
  KCD2S_CHECK(!bindings.item_from(9, current));
  bindings.bind(owner, current);
  KCD2S_CHECK(!bindings.token_for(item.uid, current));
  owner.right_token = 0;  // legacy field: visual identity only
  current[0] = item;
  bindings.bind(owner, current);
  KCD2S_CHECK(!bindings.item_from(9, current));
  KCD2S_CHECK(!bindings.token_for(item.uid, current));
}

KCD2S_TEST(hands, shared_two_hand_token_requires_one_validated_native_item) {
  Hands owner;
  owner.right = class_id(0xA0);
  owner.left = owner.right;
  owner.right_token = owner.left_token = 7;
  Hands decoded;
  KCD2S_CHECK(parse_hands_field(hands_field(owner), decoded) && decoded == owner);
  const LocalHandItem item{4, 0x1000, *owner.right};
  LocalHandItems current{item, item};
  HandItemBindings bindings;
  bindings.bind(owner, current);
  KCD2S_CHECK(bindings.item_from(7, current) == item.uid);
  current[1]->uid += 1;
  bindings.bind(owner, current);
  KCD2S_CHECK(!bindings.item_from(7, current));
  KCD2S_CHECK(!bindings.token_for(item.uid, current));
}

KCD2S_TEST(hands, metadata_observation_invalidates_tokens_before_native_presentation_changes) {
  Hands owner;
  owner.right = class_id(0xA0);
  owner.right_token = 51;
  const LocalHandItem held{4, 0x1000, *owner.right};
  LocalHandItems current{held, std::nullopt};
  HandItemBindings bindings;
  bindings.bind(owner, current);
  KCD2S_CHECK(bindings.item_from(51, current) == held.uid);
  // A rider's latest field can change before its native presentation can be touched.
  // Merely observing it must discard stale combat identity without moving the item.
  owner.right_token = 52;
  bindings.bind(owner, current);
  KCD2S_CHECK(!bindings.item_from(51, current));
  KCD2S_CHECK(bindings.item_from(52, current) == held.uid);
  KCD2S_CHECK(current[0] == held);
  owner.right = class_id(0x10);
  owner.right_token = 53;
  bindings.bind(owner, current);
  KCD2S_CHECK(!bindings.item_from(52, current));
  KCD2S_CHECK(!bindings.item_from(53, current));
  KCD2S_CHECK(current[0] == held);
  current[0] = LocalHandItem{5, 0x2000, *owner.right};
  KCD2S_CHECK(!bindings.item_from(53, current));
  bindings.bind(owner, current);
  KCD2S_CHECK(bindings.item_from(53, current) == 5);
}

// The class as scripts see it (Player.equipment, GroundItem.itemClass):
// lower-case, the 16 bytes read as two little-endian 64-bit numbers.
KCD2S_TEST(hands, script_text_is_the_reference_form) {
  // longSwordDuel, 00cca9e3-8ef2-46db-8cbf-86ec51930919, as the game keeps it.
  const ItemClassId sword{0xE3, 0xA9, 0xCC, 0x00, 0xF2, 0x8E, 0xDB, 0x46,
                          0x8C, 0xBF, 0x86, 0xEC, 0x51, 0x93, 0x09, 0x19};
  KCD2S_CHECK(item_class_script_text(sword) == "46db8ef200cca9e319099351ec86bf8c");
  ItemClassId parsed{};
  KCD2S_CHECK(parse_item_class_script_text("46DB8EF200CCA9E319099351EC86BF8C", parsed) &&
              parsed == sword);
  KCD2S_CHECK(!parse_item_class_script_text("46db8ef200cca9e319099351ec86bf8", parsed));
  KCD2S_CHECK(!parse_item_class_script_text("x6db8ef200cca9e319099351ec86bf8c", parsed));
}

// Carry: the "carry" field names the class carried, CARRY_ORDER what to do.
KCD2S_TEST(hands, carry_field_and_order) {
  Guid basket{};
  KCD2S_CHECK(parse_guid("f864a24a-152c-462e-a8f3-6760f7ce7329", basket));
  KCD2S_CHECK(carry_field(nullptr).empty());
  KCD2S_CHECK(carry_field(&basket) == "f864a24a-152c-462e-a8f3-6760f7ce7329");
  Guid parsed{};
  KCD2S_CHECK(parse_carry_field(carry_field(&basket), parsed) && parsed == basket);
  KCD2S_CHECK(!parse_carry_field("", parsed));
  KCD2S_CHECK(!parse_carry_field("basket_full_wood", parsed));

  CarryOrder carry{CarryOrderKind::Carry, basket};
  CarryOrder decoded;
  KCD2S_CHECK(decode_carry_order(encode_carry_order(carry), decoded) && decoded == carry);
  for (const auto kind : {CarryOrderKind::PutDown, CarryOrderKind::LetGo}) {
    const auto bytes = encode_carry_order(CarryOrder{kind, {}});
    KCD2S_CHECK(bytes.size() == 2);
    KCD2S_CHECK(decode_carry_order(bytes, decoded) && decoded.kind == kind);
  }
  KCD2S_CHECK(encode_carry_order(CarryOrder{CarryOrderKind(9), {}}).empty());
  KCD2S_CHECK(!decode_carry_order({kCarryOrderTag, 9}, decoded));
  KCD2S_CHECK(!decode_carry_order({kCarryOrderTag, 1, 0}, decoded));
  KCD2S_CHECK(!decode_carry_order({kCarryOrderTag, 2, 0}, decoded));
  const CarryOrder stack{CarryOrderKind::CarryStack, {}, 4097};
  KCD2S_CHECK(decode_carry_order(encode_carry_order(stack), decoded) && decoded == stack);
  KCD2S_CHECK(encode_carry_order(CarryOrder{CarryOrderKind::CarryStack, {}, 0}).empty());
  KCD2S_CHECK(!decode_carry_order({kCarryOrderTag, 4, 0}, decoded));

  CarryPutDown put_down{basket, {1646.5f, 2151.25f, 35.75f}};
  CarryPutDown landed;
  KCD2S_CHECK(decode_carry_put_down(encode_carry_put_down(put_down), landed) && landed == put_down);
  put_down.position[2] = std::nanf("");
  KCD2S_CHECK(encode_carry_put_down(put_down).empty());
  KCD2S_CHECK(!decode_carry_put_down({kCarryReportTag, 1, 2}, landed));
}
