#include "../local/human_overlay.hpp"
#include "kcd2s/testkit.hpp"

#include <array>
#include <bit>
#include <limits>

using namespace kcd2s::local;

namespace {

HumanOverlay started(std::uint64_t sequence = 1) {
  HumanOverlay overlay;
  overlay.sequence = sequence;
  overlay.alias_hash = 0xA02512BEC7816746ULL;
  overlay.state = HumanOverlayState::Start;
  return overlay;
}

void write_word(std::string& data, std::size_t offset, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    data[offset + i] = char(value >> (8 * i));
  }
}

void write_scalar(std::string& data, std::size_t offset, float value) {
  write_word(data, offset, std::bit_cast<std::uint32_t>(value));
}

}  // namespace

KCD2S_TEST(human_overlay, round_trip_unset_start_and_stop) {
  HumanOverlay unset;
  KCD2S_CHECK(valid_human_overlay(unset));
  auto decoded = decode_human_overlay(encode_human_overlay(unset));
  KCD2S_CHECK(decoded && decoded->sequence == 0 && decoded->alias_hash == 0);
  KCD2S_CHECK(decoded->state == HumanOverlayState::Unset);
  KCD2S_CHECK_EQ(decoded->params.layer, 11u);
  KCD2S_CHECK_EQ(decoded->params.speed, 1.0f);
  KCD2S_CHECK_EQ(decoded->params.weight, 1.0f);

  auto start = started(0xFEDCBA9876543210ULL);
  start.params.transition = 0.25f;
  start.params.normalized_time = 0.75f;
  start.params.speed = 2;
  start.params.weight = 0.5f;
  start.params.token = 0xFFEEDDCC;
  start.params.flags = 0x40000004;
  start.params.user_token = 0xAABBCCDD;
  for (unsigned i = 0; i < 8; ++i) {
    start.params.user_data[i] = float(i) - 4.5f;
  }
  const auto bytes = encode_human_overlay(start);
  decoded = decode_human_overlay(bytes);
  KCD2S_CHECK_EQ(bytes.size(), kHumanOverlayWireBytes);
  KCD2S_CHECK(decoded && decoded->state == HumanOverlayState::Start);
  KCD2S_CHECK_EQ(decoded->sequence, start.sequence);
  KCD2S_CHECK_EQ(decoded->alias_hash, start.alias_hash);
  KCD2S_CHECK_EQ(decoded->params.transition, start.params.transition);
  KCD2S_CHECK_EQ(decoded->params.normalized_time, start.params.normalized_time);
  KCD2S_CHECK_EQ(decoded->params.speed, start.params.speed);
  KCD2S_CHECK_EQ(decoded->params.weight, start.params.weight);
  KCD2S_CHECK_EQ(decoded->params.token, start.params.token);
  KCD2S_CHECK_EQ(decoded->params.flags, start.params.flags);
  KCD2S_CHECK_EQ(decoded->params.user_token, start.params.user_token);
  for (unsigned i = 0; i < 8; ++i) {
    KCD2S_CHECK_EQ(decoded->params.user_data[i], start.params.user_data[i]);
  }

  HumanOverlay stop;
  stop.state = HumanOverlayState::Stop;
  stop.sequence = 9;
  decoded = decode_human_overlay(encode_human_overlay(stop));
  KCD2S_CHECK(decoded && decoded->state == HumanOverlayState::Stop);
  KCD2S_CHECK_EQ(decoded->sequence, 9u);
  KCD2S_CHECK_EQ(decoded->alias_hash, 0u);
}

KCD2S_TEST(human_overlay, exact_little_endian_wire_layout) {
  auto overlay = started(0x0123456789ABCDEFULL);
  overlay.alias_hash = 0xFEDCBA9876543210ULL;
  overlay.params.transition = 0.5f;
  const auto bytes = encode_human_overlay(overlay);
  KCD2S_CHECK_EQ(bytes.size(), 88u);
  KCD2S_CHECK_EQ(bytes.substr(0, 4), "KFA1");
  const std::array<unsigned char, 20> header = {
      0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, 0x10, 0x32,
      0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 2,    0,    0,    0,
  };
  for (std::size_t i = 0; i < header.size(); ++i) {
    KCD2S_CHECK_EQ(static_cast<unsigned char>(bytes[4 + i]), header[i]);
  }
  KCD2S_CHECK_EQ(static_cast<unsigned char>(bytes[26]), 0u);
  KCD2S_CHECK_EQ(static_cast<unsigned char>(bytes[27]), 0x3Fu);
  KCD2S_CHECK_EQ(static_cast<unsigned char>(bytes[40]), 11u);

  auto external = bytes;
  write_word(external, 44, 0x12345678);
  write_scalar(external, 84, -2.25f);
  const auto decoded = decode_human_overlay(external);
  KCD2S_CHECK(decoded);
  KCD2S_CHECK_EQ(decoded->params.token, 0x12345678u);
  KCD2S_CHECK_EQ(decoded->params.user_data[7], -2.25f);
}

KCD2S_TEST(human_overlay, sequence_and_hash_state_contract) {
  HumanOverlay overlay;
  for (const auto state :
       {HumanOverlayState::Unset, HumanOverlayState::Stop, HumanOverlayState::Start}) {
    for (const std::uint64_t sequence : {0ULL, 1ULL}) {
      for (const std::uint64_t hash : {0ULL, 0x8000000000000001ULL}) {
        overlay.state = state;
        overlay.sequence = sequence;
        overlay.alias_hash = hash;
        const bool expected = state == HumanOverlayState::Unset
                                  ? sequence == 0 && hash == 0
                                  : (state == HumanOverlayState::Stop ? sequence != 0 && hash == 0
                                                                      : sequence != 0 && hash != 0);
        KCD2S_CHECK_EQ(valid_human_overlay(overlay), expected);
        KCD2S_CHECK_EQ(!encode_human_overlay(overlay).empty(), expected);
      }
    }
  }
  overlay = started(std::numeric_limits<std::uint64_t>::max());
  overlay.alias_hash = std::numeric_limits<std::uint64_t>::max();
  const auto decoded = decode_human_overlay(encode_human_overlay(overlay));
  KCD2S_CHECK(decoded);
  KCD2S_CHECK_EQ(decoded->sequence, overlay.sequence);
  KCD2S_CHECK_EQ(decoded->alias_hash, overlay.alias_hash);

  auto malformed = encode_human_overlay(started());
  for (unsigned i = 4; i < 12; ++i) {
    malformed[i] = 0;
  }
  KCD2S_CHECK(!decode_human_overlay(malformed));
  malformed = encode_human_overlay(started());
  malformed[20] = char(HumanOverlayState::Unset);
  KCD2S_CHECK(!decode_human_overlay(malformed));
  malformed[20] = char(HumanOverlayState::Stop);
  KCD2S_CHECK(!decode_human_overlay(malformed));
  malformed = encode_human_overlay(started());
  for (unsigned i = 12; i < 20; ++i) {
    malformed[i] = 0;
  }
  KCD2S_CHECK(!decode_human_overlay(malformed));
}

KCD2S_TEST(human_overlay, malformed_envelope_and_native_types_are_refused) {
  const auto valid = encode_human_overlay(started());
  KCD2S_CHECK(!decode_human_overlay(""));
  KCD2S_CHECK(!decode_human_overlay(valid.substr(0, 87)));
  KCD2S_CHECK(!decode_human_overlay(valid + "x"));
  for (const auto offset : {0u, 3u, 22u, 23u}) {
    auto malformed = valid;
    malformed[offset] = char(-1);
    KCD2S_CHECK(!decode_human_overlay(malformed));
  }
  for (const auto state : {3u, 255u}) {
    auto malformed = valid;
    malformed[20] = char(state);
    KCD2S_CHECK(!decode_human_overlay(malformed));
  }
  for (const auto type : {1u, 2u, 255u}) {
    auto malformed = valid;
    malformed[21] = char(type);
    KCD2S_CHECK(!decode_human_overlay(malformed));
  }
  for (const auto layer : {0u, 10u, 12u, 16u, 0xFFFFFFFFu}) {
    auto malformed = valid;
    write_word(malformed, 40, layer);
    KCD2S_CHECK(!decode_human_overlay(malformed));
  }
}

KCD2S_TEST(human_overlay, nonfinite_parameters_are_refused_in_every_state) {
  const std::array<float, 3> nonfinite = {
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
  };
  for (const auto state :
       {HumanOverlayState::Unset, HumanOverlayState::Stop, HumanOverlayState::Start}) {
    auto overlay = started();
    overlay.state = state;
    if (state != HumanOverlayState::Start) {
      overlay.alias_hash = 0;
    }
    if (state == HumanOverlayState::Unset) {
      overlay.sequence = 0;
    }
    const auto valid = encode_human_overlay(overlay);
    for (const auto offset : {24u, 28u, 32u, 36u, 56u, 60u, 64u, 68u, 72u, 76u, 80u, 84u}) {
      for (float value : nonfinite) {
        auto malformed = valid;
        write_scalar(malformed, offset, value);
        KCD2S_CHECK(!decode_human_overlay(malformed));
      }
    }
    overlay.params.speed = nonfinite[0];
    KCD2S_CHECK(!valid_human_overlay(overlay));
    KCD2S_CHECK(encode_human_overlay(overlay).empty());
  }
}

KCD2S_TEST(human_overlay, finite_parameter_bounds_match_animation_contract) {
  auto overlay = started();
  overlay.params.transition = 30;
  overlay.params.normalized_time = 1;
  overlay.params.speed = 100;
  overlay.params.weight = 100;
  overlay.params.user_data[0] = -std::numeric_limits<float>::max();
  overlay.params.user_data[7] = std::numeric_limits<float>::max();
  const auto valid = encode_human_overlay(overlay);
  KCD2S_CHECK(decode_human_overlay(valid));

  const std::array<std::pair<unsigned, float>, 8> outside = {{
      {24, -0.01f},
      {24, 30.01f},
      {28, -0.01f},
      {28, 1.01f},
      {32, -0.01f},
      {32, 100.01f},
      {36, -0.01f},
      {36, 100.01f},
  }};
  for (const auto& [offset, value] : outside) {
    auto malformed = valid;
    write_scalar(malformed, offset, value);
    KCD2S_CHECK(!decode_human_overlay(malformed));
  }
  overlay.params.transition = 0;
  overlay.params.normalized_time = 0;
  overlay.params.speed = 0;
  overlay.params.weight = 0;
  KCD2S_CHECK(decode_human_overlay(encode_human_overlay(overlay)));
}

KCD2S_TEST(human_overlay, start_stop_restart_keep_authoritative_ordering_data) {
  auto start = started(10);
  auto stop = start;
  stop.sequence = 11;
  stop.state = HumanOverlayState::Stop;
  stop.alias_hash = 0;
  auto restart = start;
  restart.sequence = 12;
  const auto first = decode_human_overlay(encode_human_overlay(start));
  const auto stopped = decode_human_overlay(encode_human_overlay(stop));
  const auto second = decode_human_overlay(encode_human_overlay(restart));
  KCD2S_CHECK(first && stopped && second);
  KCD2S_CHECK_EQ(first->alias_hash, second->alias_hash);
  KCD2S_CHECK(first->sequence < stopped->sequence && stopped->sequence < second->sequence);
  KCD2S_CHECK(stopped->state == HumanOverlayState::Stop);
  KCD2S_CHECK(first->state == HumanOverlayState::Start &&
              second->state == HumanOverlayState::Start);

  // The codec preserves duplicate and out-of-order sequence values; the engine-side keeper
  // can reject them without treating a restart of the same alias as an unchanged animation.
  const auto duplicate = decode_human_overlay(encode_human_overlay(stop));
  KCD2S_CHECK(duplicate && duplicate->sequence == stopped->sequence);
  const auto late_start = decode_human_overlay(encode_human_overlay(start));
  KCD2S_CHECK(late_start && late_start->sequence < second->sequence);
}
