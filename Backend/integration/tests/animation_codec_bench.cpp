// Explicit benchmark over captured native animation payloads, never fabricated clip data.
#include "../local/animation.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>

namespace {

using namespace kcd2s::local;
constexpr unsigned kIterations = 20000;

bool same_state(const AnimationState& first, const AnimationState& second) {
  for (unsigned i = 0; i < first.size(); ++i) {
    if (first[i].sequence != second[i].sequence || first[i].clip != second[i].clip ||
        std::memcmp(&first[i].params, &second[i].params, sizeof(AnimationParams)) != 0) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> read_payload(const char* path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return std::nullopt;
  }
  const auto size = file.tellg();
  if (size < 8 || size > 1220) {
    return std::nullopt;
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  file.seekg(0);
  if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
    return std::nullopt;
  }
  return bytes;
}

struct Timing {
  double encode_ns = 0;
  double decode_ns = 0;
  std::uint64_t checksum = 0;
};

Timing measure(const AnimationState& state, AnimationEncoding encoding, const std::string& bytes) {
  Timing result;
  auto start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < kIterations; ++i) {
    result.checksum += encode_animation(state, encoding).size();
  }
  result.encode_ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() /
      kIterations;
  start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < kIterations; ++i) {
    const auto decoded = decode_animation(bytes);
    if (!decoded) {
      return {};
    }
    for (const auto& layer : *decoded) {
      result.checksum += layer.sequence + std::uint32_t(layer.clip);
    }
  }
  result.decode_ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() /
      kIterations;
  return result;
}

bool report(const char* path, unsigned sample) {
  const auto input = read_payload(path);
  const auto state = input ? decode_animation(*input) : std::nullopt;
  if (!state) {
    std::fprintf(stderr, "Sample %u is not a valid raw animation payload (8..1220 bytes).\n",
                 sample);
    return false;
  }
  const auto layer_bytes = encode_animation(*state, AnimationEncoding::LayerSparse);
  const auto scalar_bytes = encode_animation(*state, AnimationEncoding::ScalarSparse);
  const auto layer_state = decode_animation(layer_bytes);
  const auto scalar_state = decode_animation(scalar_bytes);
  if (!layer_state || !scalar_state || !same_state(*state, *layer_state) ||
      !same_state(*state, *scalar_state)) {
    std::fprintf(stderr, "Sample %u failed exact parameter-bit parity.\n", sample);
    return false;
  }
  std::array<unsigned, 17> histogram{};
  unsigned present = 0;
  const AnimationParams defaults;
  std::array<std::uint32_t, 16> default_words;
  std::memcpy(default_words.data(), &defaults, sizeof(defaults));
  for (const auto& layer : *state) {
    if (default_animation_layer(layer)) {
      continue;
    }
    ++present;
    std::array<std::uint32_t, 16> words;
    std::memcpy(words.data(), &layer.params, sizeof(layer.params));
    unsigned count = 0;
    for (unsigned i = 0; i < words.size(); ++i) {
      count += words[i] != default_words[i];
    }
    ++histogram[count];
  }
  // Alternate which format runs first across samples to reduce ordering bias.
  Timing old_timing, new_timing;
  if (sample % 2) {
    new_timing = measure(*state, AnimationEncoding::ScalarSparse, scalar_bytes);
    old_timing = measure(*state, AnimationEncoding::LayerSparse, layer_bytes);
  } else {
    old_timing = measure(*state, AnimationEncoding::LayerSparse, layer_bytes);
    new_timing = measure(*state, AnimationEncoding::ScalarSparse, scalar_bytes);
  }
  std::printf(
      "ANIMATION_CODEC_BENCH {\"sample\":%u,\"input_format\":\"%.4s\",\"input_bytes\":%zu,"
      "\"present_layers\":%u,\"layer_sparse_bytes\":%zu,\"scalar_sparse_bytes\":%zu,"
      "\"selected_format\":\"%.4s\",\"parameter_word_histogram\":[",
      sample, input->data(), input->size(), present, layer_bytes.size(), scalar_bytes.size(),
      scalar_bytes.data());
  for (unsigned i = 0; i < histogram.size(); ++i) {
    std::printf("%s%u", i ? "," : "", histogram[i]);
  }
  std::printf(
      "],\"iterations\":%u,\"layer_encode_ns\":%.3f,\"scalar_encode_ns\":%.3f,"
      "\"layer_decode_ns\":%.3f,\"scalar_decode_ns\":%.3f,\"bit_parity\":true,"
      "\"checksum\":%llu}\n",
      kIterations, old_timing.encode_ns, new_timing.encode_ns, old_timing.decode_ns,
      new_timing.decode_ns,
      static_cast<unsigned long long>(old_timing.checksum + new_timing.checksum));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 257) {
    std::fprintf(stderr,
                 "Usage: bench_integration_animation_codec <raw-KSA-file> [more raw files]\n"
                 "Supply 1..256 locally captured native payloads; no synthetic fallback.\n");
    return 2;
  }
  for (int i = 1; i < argc; ++i) {
    if (!report(argv[i], static_cast<unsigned>(i - 1))) {
      return 1;
    }
  }
  return 0;
}
