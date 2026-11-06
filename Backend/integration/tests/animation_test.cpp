#include "../local/animation.hpp"
#include "kcd2s/testkit.hpp"
#include <limits>
using namespace kcd2s;

namespace {
bool same_state(const local::AnimationState& first, const local::AnimationState& second) {
  for (unsigned i = 0; i < first.size(); ++i) {
    if (first[i].sequence != second[i].sequence || first[i].clip != second[i].clip ||
        std::memcmp(&first[i].params, &second[i].params, sizeof(local::AnimationParams)) != 0) {
      return false;
    }
  }
  return true;
}
}  // namespace

KCD2S_TEST(animation, sparse_common_state_saves_bytes_without_changing_any_parameter) {
  local::AnimationState state{};
  const auto empty = local::encode_animation(state, local::AnimationEncoding::LayerSparse);
  KCD2S_CHECK_EQ(empty.size(), std::size_t{8});
  KCD2S_CHECK(empty.starts_with("KSA2"));
  const auto empty_read = local::decode_animation(empty);
  KCD2S_CHECK(empty_read && same_state(state, *empty_read));
  auto& layer = state[5];
  layer.sequence = 0xFEDCBA9876543210ULL;
  layer.clip = 321;
  layer.params.transition = 0.125f;
  layer.params.normalized_time = 0.625f;
  layer.params.speed = 1.25f;
  layer.params.weight = 0.75f;
  layer.params.layer = 5;
  layer.params.token = 65535;
  layer.params.flags = 0x80000004;
  layer.params.user_token = 0x12345678;
  for (unsigned i = 0; i < 8; ++i) {
    layer.params.user_data[i] = float(i) - 3.25f;
  }
  const auto encoded = local::encode_animation(state, local::AnimationEncoding::LayerSparse);
  KCD2S_CHECK_EQ(encoded.size(), std::size_t{84});
  KCD2S_CHECK(encoded.starts_with("KSA2"));
  const auto decoded = local::decode_animation(encoded);
  KCD2S_CHECK(decoded && same_state(state, *decoded));
  KCD2S_CHECK(encoded.size() * 10 < std::size_t{1220});
}

KCD2S_TEST(animation, sparse_state_keeps_stops_and_nondefault_unsequenced_layers) {
  local::AnimationState state{};
  state[2].sequence = 19;
  state[2].clip = -1;
  state[2].params.layer = 2;
  state[7].params.layer = 7;
  state[9].params.normalized_time = -0.0f;
  state[13].clip = 10;
  const auto bytes = local::encode_animation(state, local::AnimationEncoding::LayerSparse);
  KCD2S_CHECK_EQ(bytes.size(), std::size_t{8 + 4 * 76});
  const auto decoded = local::decode_animation(bytes);
  KCD2S_CHECK(decoded && same_state(state, *decoded));
}

KCD2S_TEST(animation, layer_sparse_full_state_and_legacy_defaults_decode_exactly) {
  local::AnimationState state{};
  for (unsigned i = 0; i < state.size(); ++i) {
    state[i].sequence = i + 1;
    state[i].clip = i == 15 ? -1 : std::int32_t(i + 100);
    state[i].params.layer = i;
    state[i].params.normalized_time = float(i) / 16;
  }
  const auto bytes = local::encode_animation(state, local::AnimationEncoding::LayerSparse);
  KCD2S_CHECK_EQ(bytes.size(), std::size_t{1220});
  KCD2S_CHECK(bytes.starts_with("KSA1"));
  const auto decoded = local::decode_animation(bytes);
  KCD2S_CHECK(decoded && same_state(state, *decoded));
  // A real old encoder sends default layers in KSA1 too. Replace one complete
  // layer's bytes with a default layer while retaining the old packet layout.
  local::AnimationState old_default{};
  const auto sparse_default = local::encode_animation(old_default);
  KCD2S_CHECK_EQ(sparse_default.size(), std::size_t{8});
  state[0] = {};
  auto legacy = bytes;
  std::string default_words(76, '\0');
  default_words.replace(8, 4, std::string(4, '\xFF'));
  default_words[22] = '\x80';
  default_words[23] = '\x3F';
  default_words[26] = '\x80';
  default_words[27] = '\x3F';
  legacy.replace(4, 76, default_words);
  const auto old_decoded = local::decode_animation(legacy);
  KCD2S_CHECK(old_decoded && same_state(state, *old_decoded));
}

KCD2S_TEST(animation, sparse_rejects_truncation_unknown_mask_bits_and_extra_layers) {
  local::AnimationState state{};
  state[0].sequence = 1;
  state[0].clip = 21;
  const auto bytes = local::encode_animation(state);
  for (std::size_t size = 0; size < bytes.size(); ++size) {
    KCD2S_CHECK(!local::decode_animation(bytes.substr(0, size)));
  }
  KCD2S_CHECK(!local::decode_animation(bytes + "x"));
  auto invalid = bytes;
  invalid[6] = 1;  // mask bit 16 has no layer in this protocol.
  KCD2S_CHECK(!local::decode_animation(invalid));
  invalid = bytes;
  invalid[4] = 3;  // two layers claimed, only one present.
  KCD2S_CHECK(!local::decode_animation(invalid));
  invalid = bytes;
  invalid[3] = '4';
  KCD2S_CHECK(!local::decode_animation(invalid));
}

KCD2S_TEST(animation, scalar_sparse_has_exact_scalar_layout_and_preserves_max_sequence) {
  local::AnimationState state{};
  auto& layer = state[0];
  layer.sequence = std::numeric_limits<std::uint64_t>::max();
  layer.clip = 65535;
  layer.params.normalized_time = 0.625f;
  layer.params.flags = 0x80000004;
  const auto bytes = local::encode_animation(state, local::AnimationEncoding::ScalarSparse);
  KCD2S_CHECK(bytes.starts_with("KSA3"));
  KCD2S_CHECK_EQ(bytes.size(), std::size_t{30});
  KCD2S_CHECK_EQ(static_cast<unsigned char>(bytes[20]), 0x42);
  KCD2S_CHECK_EQ(static_cast<unsigned char>(bytes[21]), 0);
  // 0.625f is 0x3F200000 and flags are 0x80000004, both little-endian.
  KCD2S_CHECK(bytes.substr(22) == std::string("\x00\x00\x20\x3F\x04\x00\x00\x80", 8));
  const auto decoded = local::decode_animation(bytes);
  KCD2S_CHECK(decoded && same_state(state, *decoded));
}

KCD2S_TEST(animation, scalar_sparse_keeps_stops_and_every_unsequenced_parameter_bit) {
  constexpr std::array<std::uint32_t, 10> patterns{0,          0x80000000, 0x7FC12345, 0xFFCABCDE,
                                                   1,          0xFFFFFFFF, 0x7F800000, 0xFF800000,
                                                   0x3F800000, 0x12345678};
  for (unsigned parameter = 0; parameter < 16; ++parameter) {
    for (const auto pattern : patterns) {
      local::AnimationState state{};
      state[2].sequence = 19;
      state[2].clip = -1;
      state[2].params.layer = 2;
      // No sequence means opaque capture data. Preserve even unknown clip IDs,
      // NaN payloads and signed zero; they are not live native start requests.
      state[13].clip = std::numeric_limits<std::int32_t>::min();
      std::memcpy(reinterpret_cast<unsigned char*>(&state[13].params) + 4 * parameter, &pattern,
                  sizeof(pattern));
      const auto bytes = local::encode_animation(state, local::AnimationEncoding::ScalarSparse);
      KCD2S_CHECK(bytes.size() <= std::size_t{1220});
      const auto decoded = local::decode_animation(bytes);
      KCD2S_CHECK(decoded && same_state(state, *decoded));
    }
  }
}

KCD2S_TEST(animation, scalar_sparse_selects_legacy_when_parameter_words_are_dense) {
  local::AnimationState state{};
  for (unsigned i = 0; i < state.size(); ++i) {
    auto& layer = state[i];
    layer.sequence = i + 1;
    layer.clip = std::int32_t(200 + i);
    layer.params.transition = 0.125f;
    layer.params.normalized_time = 0.625f;
    layer.params.speed = 1.25f;
    layer.params.weight = 0.75f;
    layer.params.layer = i;
    layer.params.token = 6;
    layer.params.flags = 7;
    layer.params.user_token = 8;
    for (unsigned j = 0; j < 8; ++j) {
      layer.params.user_data[j] = float(j + 1);
    }
  }
  const auto full = local::encode_animation(state, local::AnimationEncoding::ScalarSparse);
  KCD2S_CHECK_EQ(full.size(), std::size_t{1220});
  KCD2S_CHECK(full.starts_with("KSA1"));
  const auto full_decoded = local::decode_animation(full);
  KCD2S_CHECK(full_decoded && same_state(state, *full_decoded));
  for (unsigned i = 0; i < state.size(); ++i) {
    if (i != 5) {
      state[i] = {};
    }
  }
  const auto one = local::encode_animation(state, local::AnimationEncoding::ScalarSparse);
  KCD2S_CHECK_EQ(one.size(), std::size_t{84});
  KCD2S_CHECK(one.starts_with("KSA2"));
  const auto one_decoded = local::decode_animation(one);
  KCD2S_CHECK(one_decoded && same_state(state, *one_decoded));
  const auto empty = local::encode_animation({}, local::AnimationEncoding::ScalarSparse);
  KCD2S_CHECK_EQ(empty.size(), std::size_t{8});
  KCD2S_CHECK(empty.starts_with("KSA2"));  // Equal size retains the previous format.
}

KCD2S_TEST(animation, scalar_sparse_all_layers_are_bounded_and_decoder_refuses_missing_words) {
  local::AnimationState state{};
  for (unsigned i = 0; i < state.size(); ++i) {
    state[i].sequence = i + 1;
    state[i].clip = i == 15 ? -1 : std::int32_t(i + 100);
    state[i].params.layer = i;
    state[i].params.normalized_time = float(i) / 16;
  }
  const auto bytes = local::encode_animation(state, local::AnimationEncoding::ScalarSparse);
  KCD2S_CHECK(bytes.starts_with("KSA3"));
  KCD2S_CHECK_EQ(bytes.size(), std::size_t{352});
  const auto decoded = local::decode_animation(bytes);
  KCD2S_CHECK(decoded && same_state(state, *decoded));
  for (std::size_t size = 0; size < bytes.size(); ++size) {
    KCD2S_CHECK(!local::decode_animation(bytes.substr(0, size)));
  }
  KCD2S_CHECK(!local::decode_animation(bytes + "x"));
  auto invalid = bytes;
  invalid[6] = 1;  // Layer presence has no bit16.
  KCD2S_CHECK(!local::decode_animation(invalid));
  // A sole layer whose parameter mask claims one word has no word bytes.
  invalid = std::string("KSA3\x01\x00\x00\x00", 8) + std::string(14, '\0');
  invalid[20] = 1;
  KCD2S_CHECK(!local::decode_animation(invalid));
  invalid[20] = 0;
  invalid[21] = char(0x80);  // Highest parameter bit needs its word too.
  KCD2S_CHECK(!local::decode_animation(invalid));
  // A valid length does not bypass native parameter validation.
  state[3].params.layer = 16;
  KCD2S_CHECK(!local::decode_animation(
      local::encode_animation(state, local::AnimationEncoding::ScalarSparse)));
  state[3].params.layer = 3;
  state[3].params.speed = std::numeric_limits<float>::infinity();
  KCD2S_CHECK(!local::decode_animation(
      local::encode_animation(state, local::AnimationEncoding::ScalarSparse)));
}

KCD2S_TEST(animation, scalar_sparse_mixed_layer_masks_roundtrip_without_exceeding_old_size) {
  std::uint32_t random = 0x4875A3u;
  auto next = [&]() {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  for (unsigned sample = 0; sample < 512; ++sample) {
    local::AnimationState state{};
    for (unsigned layer = 0; layer < state.size(); ++layer) {
      if ((next() & 3) == 0) {
        continue;
      }
      auto& current = state[layer];
      // Unsequenced states can contain arbitrary captured bits, so this covers
      // dense and sparse word masks without accidentally constraining the data.
      current.clip = std::bit_cast<std::int32_t>(next());
      for (unsigned parameter = 0; parameter < 16; ++parameter) {
        if ((next() & 1) != 0) {
          const auto bits = next();
          std::memcpy(reinterpret_cast<unsigned char*>(&current.params) + 4 * parameter, &bits,
                      sizeof(bits));
        }
      }
    }
    const auto previous = local::encode_animation(state, local::AnimationEncoding::LayerSparse);
    const auto compact = local::encode_animation(state, local::AnimationEncoding::ScalarSparse);
    KCD2S_CHECK(compact.size() <= previous.size());
    KCD2S_CHECK(compact.size() <= std::size_t{1220});
    const auto decoded = local::decode_animation(compact);
    KCD2S_CHECK(decoded && same_state(state, *decoded));
  }
}

KCD2S_TEST(animation, bounded_binary_state) {
  local::AnimationState state{};
  auto& a = state[3];
  a.sequence = 0x100000002ULL;
  a.clip = 942;
  a.params.layer = 3;
  a.params.flags = 0x40000004;
  a.params.token = 771;
  a.params.user_data[2] = -0.75f;
  const auto bytes = local::encode_animation(state);
  auto decoded = local::decode_animation(bytes);
  KCD2S_CHECK(decoded.has_value());
  KCD2S_CHECK_EQ((*decoded)[3].sequence, a.sequence);
  KCD2S_CHECK_EQ((*decoded)[3].clip, 942);
  KCD2S_CHECK_EQ((*decoded)[3].params.flags, a.params.flags);
  KCD2S_CHECK_EQ((*decoded)[3].params.user_data[2], -0.75f);
  KCD2S_CHECK(!local::decode_animation(bytes.substr(0, bytes.size() - 1)));
  KCD2S_CHECK(!local::decode_animation(bytes + "x"));
  auto invalid = bytes;
  invalid[0] = 'X';
  KCD2S_CHECK(!local::decode_animation(invalid));
  a.params.layer = 16;
  KCD2S_CHECK(!local::decode_animation(local::encode_animation(state)));
  a.params.layer = 3;
  a.params.speed = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(!local::decode_animation(local::encode_animation(state)));
}
KCD2S_TEST(animation, replay_stop_and_restart_sequences) {
  local::AnimationState state{};
  state[0].sequence = 1;
  state[0].clip = 53;
  auto first = local::decode_animation(local::encode_animation(state));
  state[0].sequence = 2;
  state[0].clip = -1;
  auto stop = local::decode_animation(local::encode_animation(state));
  state[0].sequence = 3;
  state[0].clip = 53;
  auto restart = local::decode_animation(local::encode_animation(state));
  KCD2S_CHECK(first && stop && restart);
  KCD2S_CHECK_EQ((*stop)[0].clip, -1);
  KCD2S_CHECK((*restart)[0].sequence > (*first)[0].sequence);
}

KCD2S_TEST(animation, stopped_fifo_is_not_a_second_attack) {
  local::AnimationStopFence fence;
  KCD2S_CHECK(!fence.fading(6, 16193));
  fence.stop(6, 16193);
  KCD2S_CHECK(fence.fading(6, 16193));
  fence.stop(6, -1);
  KCD2S_CHECK(fence.fading(6, 16193));
  KCD2S_CHECK(!fence.fading(6, 5617));
  KCD2S_CHECK(!fence.fading(7, 16193));
  fence.start(6);
  KCD2S_CHECK(!fence.fading(6, 16193));
}
