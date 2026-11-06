#include "kcd2s/core/quant.hpp"

#include <cmath>
#include <random>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s::core;

KCD2S_TEST(quant, float_roundtrip_within_error_bound) {
  const float min = -40.0f, max = 40.0f;
  const int bits = 16;
  const float bound = float_quant_max_error(min, max, bits);
  std::mt19937 rng(1);
  std::uniform_real_distribution<float> d(min, max);
  for (int i = 0; i < 2000; ++i) {
    const float v = d(rng);
    BitWriter w;
    write_float_quant(w, v, min, max, bits);
    BitReader r(w.bytes());
    const float back = read_float_quant(r, min, max, bits);
    KCD2S_CHECK(std::fabs(back - v) <= bound + 1e-4f);
  }
}

KCD2S_TEST(quant, float_endpoints_exact_ish) {
  const float min = 0.0f, max = 1.0f;
  const int bits = 10;
  BitWriter w;
  write_float_quant(w, min, min, max, bits);
  write_float_quant(w, max, min, max, bits);
  BitReader r(w.bytes());
  KCD2S_CHECK(std::fabs(read_float_quant(r, min, max, bits) - min) < 1e-3f);
  KCD2S_CHECK(std::fabs(read_float_quant(r, min, max, bits) - max) < 1e-3f);
}

KCD2S_TEST(quant, float_clamps_out_of_range) {
  const float min = 0.0f, max = 1.0f;
  const int bits = 8;
  BitWriter w;
  write_float_quant(w, 5.0f, min, max, bits);   // above
  write_float_quant(w, -5.0f, min, max, bits);  // below
  BitReader r(w.bytes());
  KCD2S_CHECK(read_float_quant(r, min, max, bits) <= max + 1e-3f);
  KCD2S_CHECK(read_float_quant(r, min, max, bits) >= min - 1e-3f);
}

static Quat normalize(Quat q) {
  const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return Quat{q.x / n, q.y / n, q.z / n, q.w / n};
}
static float adot(const Quat& a, const Quat& b) {
  return std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
}

KCD2S_TEST(quant, unit_quat_roundtrip_preserves_rotation) {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> d(-1.0f, 1.0f);
  const int qbits = 12;
  for (int i = 0; i < 2000; ++i) {
    Quat q = normalize(Quat{d(rng), d(rng), d(rng), d(rng)});
    BitWriter w;
    write_unit_quat(w, q, qbits);
    BitReader r(w.bytes());
    const Quat back = read_unit_quat(r, qbits);
    // q and back represent the same rotation iff |dot| ~ 1.
    KCD2S_CHECK(adot(q, back) > 0.999f);
    // result stays ~unit length
    const float len =
        std::sqrt(back.x * back.x + back.y * back.y + back.z * back.z + back.w * back.w);
    KCD2S_CHECK(std::fabs(len - 1.0f) < 1e-2f);
  }
}

KCD2S_TEST(quant, unit_quat_identity) {
  BitWriter w;
  write_unit_quat(w, Quat{0, 0, 0, 1}, 10);
  BitReader r(w.bytes());
  const Quat back = read_unit_quat(r, 10);
  KCD2S_CHECK(adot(Quat{0, 0, 0, 1}, back) > 0.999f);
}
