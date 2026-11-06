#include "kcd2s/core/ids.hpp"

#include <cstdint>
#include <string_view>

#include "kcd2s/testkit.hpp"

using namespace kcd2s::core;

static_assert(kFirstEntityId != kInvalidEntity, "the first sequential id must be valid");
static_assert(kLastEntityId != kInvalidEntity, "ids must never wrap to the invalid id");

KCD2S_TEST(ids, sequential_id_range_excludes_invalid) {
  KCD2S_CHECK_EQ(kFirstEntityId, EntityId{1});
  KCD2S_CHECK_EQ(kLastEntityId, EntityId{0xFFFFFFFFu});
}

KCD2S_TEST(ids, crc32_known_vector) {
  // CRC-32/ISO-HDLC of "123456789" is the standard check value 0xCBF43926.
  const char* s = "123456789";
  KCD2S_CHECK_EQ(crc32(s, 9), std::uint32_t{0xCBF43926u});
}

KCD2S_TEST(ids, crc32_empty_is_zero) {
  KCD2S_CHECK_EQ(crc32("", 0), std::uint32_t{0});
}

KCD2S_TEST(ids, type_id_stable_and_distinct) {
  const TypeId human = type_id("Human");
  const TypeId human2 = type_id(std::string_view("Human"));
  const TypeId vehicle = type_id("Vehicle");
  KCD2S_CHECK_EQ(human, human2);  // deterministic: client and server agree
  KCD2S_CHECK(human != vehicle);
}
