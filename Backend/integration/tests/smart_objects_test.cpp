#include "kcd2s/integration/smart_objects.hpp"

#include <cmath>

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

KCD2S_TEST(smart_objects, template_guid_is_the_windows_layout) {
  std::array<std::uint8_t, 16> guid{};
  KCD2S_CHECK(parse_template_guid("F6A5D971-3544-415b-a75a-33bd9dcac9e4", guid));
  const std::array<std::uint8_t, 16> expected = {0x71, 0xd9, 0xa5, 0xf6, 0x44, 0x35, 0x5b, 0x41,
                                                 0xa7, 0x5a, 0x33, 0xbd, 0x9d, 0xca, 0xc9, 0xe4};
  KCD2S_CHECK(guid == expected);
  KCD2S_CHECK(!parse_template_guid("f6a5d971-3544-415b-a75a-33bd9dcac9e", guid));
  KCD2S_CHECK(!parse_template_guid("f6a5d971x3544-415b-a75a-33bd9dcac9e4", guid));
  KCD2S_CHECK(!parse_template_guid("g6a5d971-3544-415b-a75a-33bd9dcac9e4", guid));
}

KCD2S_TEST(smart_objects, order_round_trips) {
  static_assert(kSmartObjectTag == 158);
  SmartObjectOrder order;
  order.id = 4114;
  KCD2S_CHECK(parse_template_guid("f6a5d971-3544-415b-a75a-33bd9dcac9e4", order.template_guid));
  order.position[0] = 1650.5f;
  order.rotation[2] = 0.7071068f;
  order.rotation[3] = 0.7071068f;
  SmartObjectOrder decoded;
  KCD2S_CHECK(decode_smart_object(encode_smart_object(order), decoded));
  KCD2S_CHECK(decoded == order);
  // The dice table's behavior, placed where it stands in the prefab (a brush is its parent).
  SmartObjectCorrection correction;
  correction.place = 6;
  correction.at[0] = 1.6111f;
  correction.at[1] = -0.009f;
  correction.turn[3] = 1.0f;
  order.corrections.push_back(correction);
  KCD2S_CHECK(decode_smart_object(encode_smart_object(order), decoded));
  KCD2S_CHECK(decoded == order);
  order.corrections[0].at[2] = 100.0f;
  KCD2S_CHECK(encode_smart_object(order).empty());
  order.corrections.assign(kMaxSmartObjectCorrections + 1, correction);
  KCD2S_CHECK(encode_smart_object(order).empty());
  order.corrections.clear();
  SmartObjectOrder down;
  down.id = 4114;
  down.remove = true;
  KCD2S_CHECK(decode_smart_object(encode_smart_object(down), decoded));
  KCD2S_CHECK(decoded == down);
}

KCD2S_TEST(smart_objects, order_refuses_bad_input) {
  SmartObjectOrder order;
  order.id = 4114;
  KCD2S_CHECK(encode_smart_object(order).empty());  // no template
  KCD2S_CHECK(parse_template_guid("f6a5d971-3544-415b-a75a-33bd9dcac9e4", order.template_guid));
  order.position[1] = std::nanf("");
  KCD2S_CHECK(encode_smart_object(order).empty());
  order.position[1] = 20000.0f;
  KCD2S_CHECK(encode_smart_object(order).empty());
  order.position[1] = 0.0f;
  order.rotation[0] = 3.0f;
  KCD2S_CHECK(encode_smart_object(order).empty());
  order.rotation[0] = 0.0f;
  order.id = 0;
  KCD2S_CHECK(encode_smart_object(order).empty());
  order.id = 4114;
  auto cut = encode_smart_object(order);
  cut.resize(cut.size() - 2);
  SmartObjectOrder decoded;
  KCD2S_CHECK(!decode_smart_object(cut, decoded));
}
