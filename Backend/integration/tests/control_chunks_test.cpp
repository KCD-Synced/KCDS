#include "kcd2s/integration/control_chunks.hpp"

#include <cstdint>
#include <vector>

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

namespace {

std::vector<std::uint8_t> message_of(std::size_t size) {
  std::vector<std::uint8_t> message(size);
  for (std::size_t i = 0; i < size; ++i) {
    message[i] = std::uint8_t(i * 31 + 7);
  }
  return message;
}

}  // namespace

KCD2S_TEST(control_chunks, a_short_message_goes_whole) {
  const auto message = message_of(kMaxWholeControlBytes);
  const auto parts = split_control_message(message);
  KCD2S_CHECK_EQ(parts.size(), std::size_t(1));
  KCD2S_CHECK(parts[0] == message);
}

KCD2S_TEST(control_chunks, a_long_message_is_joined_from_its_chunks_in_order) {
  const auto message = message_of(500000);
  const auto parts = split_control_message(message);
  KCD2S_CHECK_EQ(parts.size(), std::size_t(34));
  ControlChunks chunks;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    KCD2S_CHECK(parts[i].size() <= 9 + kControlChunkBytes);
    KCD2S_CHECK_EQ(parts[i][0], kControlChunkTag);
    const auto joined = chunks.take(parts[i]);
    if (i + 1 < parts.size()) {
      KCD2S_CHECK(!joined);
    } else {
      KCD2S_CHECK(joined && *joined == message);
    }
  }
  // Ready for the next one.
  const auto again = split_control_message(message_of(70000));
  std::optional<std::vector<std::uint8_t>> last;
  for (const auto& part : again) {
    last = chunks.take(part);
  }
  KCD2S_CHECK(last && *last == message_of(70000));
}

KCD2S_TEST(control_chunks, a_gap_or_another_message_starts_over) {
  const auto parts = split_control_message(message_of(100000));
  ControlChunks chunks;
  KCD2S_CHECK(!chunks.take(parts[0]));
  // A chunk missing: what follows does not join.
  KCD2S_CHECK(!chunks.take(parts[2]));
  KCD2S_CHECK(!chunks.take(parts[3]));
  // Pieces of another message do not join with these.
  const auto other = split_control_message(message_of(90000));
  KCD2S_CHECK(!chunks.take(parts[0]));
  KCD2S_CHECK(!chunks.take(other[1]));
  // Garbage and lengths out of bounds are refused.
  KCD2S_CHECK(!chunks.take({kControlChunkTag, 1, 2}));
  auto huge = parts[0];
  huge[4] = 0x7F;  // the whole length past kMaxJoinedControlBytes
  KCD2S_CHECK(!chunks.take(huge));
  // Too long to send at all.
  KCD2S_CHECK(split_control_message(message_of(kMaxJoinedControlBytes + 1)).empty());
}
