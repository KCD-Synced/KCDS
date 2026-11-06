// Quantization primitives for the wire (bit I/O). Floats compress to a fixed
// bit count over a known range; unit quaternions use smallest-three (3 components + a 2-bit index).
// These sit on top of Backend/core/bits and are what the schema codec emits per field.
#ifndef KCD2S_CORE_QUANT_HPP
#define KCD2S_CORE_QUANT_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/math.hpp"

namespace kcd2s::core {

// Map v in [min,max] to a `bits`-wide unsigned integer (bits in [1,32]).
inline std::uint32_t quantize_float(float v, float min, float max, int bits) {
  const float range = max - min;
  if (range <= 0.0f || bits <= 0)
    return 0;
  const std::uint32_t maxq = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
  float t = (v - min) / range;
  t = std::clamp(t, 0.0f, 1.0f);
  return static_cast<std::uint32_t>(std::lround(t * static_cast<double>(maxq)));
}

inline float dequantize_float(std::uint32_t q, float min, float max, int bits) {
  const float range = max - min;
  if (range <= 0.0f || bits <= 0)
    return min;
  const std::uint32_t maxq = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
  return min + (static_cast<float>(q) / static_cast<float>(maxq)) * range;
}

inline void write_float_quant(BitWriter& bw, float v, float min, float max, int bits) {
  bw.write_bits(quantize_float(v, min, max, bits), bits);
}
inline float read_float_quant(BitReader& br, float min, float max, int bits) {
  return dequantize_float(static_cast<std::uint32_t>(br.read_bits(bits)), min, max, bits);
}

// Worst-case absolute error of a round-trip float quantization over [min,max] with `bits`.
inline float float_quant_max_error(float min, float max, int bits) {
  const std::uint32_t maxq = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
  return (max - min) / static_cast<float>(maxq) * 0.5f;
}

// Smallest-three unit-quaternion compression: drop the largest-magnitude component (reconstructed
// from the unit constraint), store its index in 2 bits and the other three in [-1/√2, 1/√2] each
// with `qbits`. The quaternion is sign-normalized so the dropped component is non-negative (q and
// -q are the same rotation), resolving the sign ambiguity.
inline void write_unit_quat(BitWriter& bw, const Quat& q, int qbits) {
  const float c[4] = {q.x, q.y, q.z, q.w};
  int maxi = 0;
  for (int i = 1; i < 4; ++i)
    if (std::fabs(c[i]) > std::fabs(c[maxi]))
      maxi = i;
  const float sign = (c[maxi] < 0.0f) ? -1.0f : 1.0f;
  const float K = 0.7071067811865476f;  // 1/sqrt(2)
  bw.write_bits(static_cast<std::uint64_t>(maxi), 2);
  for (int i = 0; i < 4; ++i)
    if (i != maxi)
      write_float_quant(bw, sign * c[i], -K, K, qbits);
}

inline Quat read_unit_quat(BitReader& br, int qbits) {
  const int maxi = static_cast<int>(br.read_bits(2));
  const float K = 0.7071067811865476f;
  float v[4] = {0, 0, 0, 0};
  float sumsq = 0.0f;
  for (int i = 0; i < 4; ++i) {
    if (i == maxi)
      continue;
    v[i] = read_float_quant(br, -K, K, qbits);
    sumsq += v[i] * v[i];
  }
  v[maxi] = std::sqrt(std::max(0.0f, 1.0f - sumsq));
  return Quat{v[0], v[1], v[2], v[3]};
}

}  // namespace kcd2s::core

#endif  // KCD2S_CORE_QUANT_HPP
