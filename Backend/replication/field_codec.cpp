#include "kcd2s/replication/field_codec.hpp"

#include <cstring>
#include <string>
#include <variant>

#include "kcd2s/core/quant.hpp"

namespace kcd2s::replication {

using core::BitReader;
using core::BitWriter;

namespace {

float as_f32(const FieldValue& v) {
  if (auto* p = std::get_if<float>(&v))
    return *p;
  return 0.0f;
}
Vec3 as_vec3(const FieldValue& v) {
  if (auto* p = std::get_if<Vec3>(&v))
    return *p;
  return Vec3{};
}
Quat as_quat(const FieldValue& v) {
  if (auto* p = std::get_if<Quat>(&v))
    return *p;
  return Quat{};
}
std::int64_t as_i64(const FieldValue& v) {
  if (auto* p = std::get_if<std::int64_t>(&v))
    return *p;
  return 0;
}
bool as_bool(const FieldValue& v) {
  if (auto* p = std::get_if<bool>(&v))
    return *p;
  return false;
}
// Strings can be large (animation state); read them in place without copying. A value of the
// wrong alternative encodes as the empty string.
const std::string& str_ref(const FieldValue& v) {
  static const std::string empty;
  if (auto* p = std::get_if<std::string>(&v))
    return *p;
  return empty;
}

void write_raw_f32(BitWriter& w, float f) {
  std::uint32_t bits;
  std::memcpy(&bits, &f, 4);
  w.write_bits(bits, 32);
}
float read_raw_f32(BitReader& r) {
  const std::uint32_t bits = static_cast<std::uint32_t>(r.read_bits(32));
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void write_f32(BitWriter& w, const Quant& q, float v) {
  if (q.bits > 0)
    core::write_float_quant(w, v, q.min, q.max, q.bits);
  else
    write_raw_f32(w, v);
}
float read_f32(BitReader& r, const Quant& q) {
  return q.bits > 0 ? core::read_float_quant(r, q.min, q.max, q.bits) : read_raw_f32(r);
}

}  // namespace

void encode_field(BitWriter& w, const FieldDef& fd, const FieldValue& v) {
  switch (fd.kind) {
    case FieldKind::F32:
      write_f32(w, fd.quant, as_f32(v));
      break;
    case FieldKind::Vec3: {
      const Vec3 p = as_vec3(v);
      write_f32(w, fd.quant, p.x);
      write_f32(w, fd.quant, p.y);
      write_f32(w, fd.quant, p.z);
      break;
    }
    case FieldKind::Quat: {
      const Quat qq = as_quat(v);
      if (fd.quant.bits > 0) {
        core::write_unit_quat(w, qq, fd.quant.bits);
      } else {
        write_raw_f32(w, qq.x);
        write_raw_f32(w, qq.y);
        write_raw_f32(w, qq.z);
        write_raw_f32(w, qq.w);
      }
      break;
    }
    case FieldKind::I64:
      w.write_varint(as_i64(v));
      break;
    case FieldKind::Bool:
      w.write_bool(as_bool(v));
      break;
    case FieldKind::Str: {
      const std::string& s = str_ref(v);
      w.write_varuint(s.size());
      w.write_byte_run(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
      break;
    }
  }
}

FieldValue decode_field(BitReader& r, const FieldDef& fd) {
  switch (fd.kind) {
    case FieldKind::F32:
      return FieldValue{read_f32(r, fd.quant)};
    case FieldKind::Vec3: {
      Vec3 p;
      p.x = read_f32(r, fd.quant);
      p.y = read_f32(r, fd.quant);
      p.z = read_f32(r, fd.quant);
      return FieldValue{p};
    }
    case FieldKind::Quat: {
      if (fd.quant.bits > 0)
        return FieldValue{core::read_unit_quat(r, fd.quant.bits)};
      Quat qq;
      qq.x = read_raw_f32(r);
      qq.y = read_raw_f32(r);
      qq.z = read_raw_f32(r);
      qq.w = read_raw_f32(r);
      return FieldValue{qq};
    }
    case FieldKind::I64:
      return FieldValue{r.read_varint()};
    case FieldKind::Bool:
      return FieldValue{r.read_bool()};
    case FieldKind::Str: {
      // Untrusted length: never resize/loop beyond the bytes actually present. Compare with a
      // division (not len*8) so a hostile varuint near 2^64 can't overflow the bound and slip
      // through into a giant allocation or a multi-exabyte read loop.
      const std::uint64_t len = r.read_varuint();
      const std::uint64_t avail = r.bits_remaining() / 8;
      const std::uint64_t take = len <= avail ? len : avail;
      std::string s;
      s.resize(static_cast<std::size_t>(take));
      r.read_byte_run(reinterpret_cast<std::uint8_t*>(s.data()), s.size());
      if (len > avail)
        (void)r.read_bits(8);  // claim exceeds buffer -> trip overrun; caller rejects
      return FieldValue{std::move(s)};
    }
  }
  return FieldValue{};
}

namespace {

// Raw floats are sent as their exact 32-bit pattern, so equality is bit equality.
bool same_raw_f32(float a, float b) {
  std::uint32_t a_bits;
  std::uint32_t b_bits;
  std::memcpy(&a_bits, &a, 4);
  std::memcpy(&b_bits, &b, 4);
  return a_bits == b_bits;
}

bool same_raw_vec3(const Vec3& a, const Vec3& b) {
  return same_raw_f32(a.x, b.x) && same_raw_f32(a.y, b.y) && same_raw_f32(a.z, b.z);
}

bool same_raw_quat(const Quat& a, const Quat& b) {
  return same_raw_f32(a.x, b.x) && same_raw_f32(a.y, b.y) && same_raw_f32(a.z, b.z) &&
         same_raw_f32(a.w, b.w);
}

// Reference definition: encode both values and compare the emitted bits.
bool same_encoded_bits(const FieldDef& fd, const FieldValue& a, const FieldValue& b) {
  BitWriter first;
  BitWriter second;
  encode_field(first, fd, a);
  encode_field(second, fd, b);
  return first.bit_count() == second.bit_count() && first.bytes() == second.bytes();
}

}  // namespace

bool same_encoding(const FieldDef& fd, const FieldValue& a, const FieldValue& b) {
  const bool quantized = fd.quant.bits > 0;
  switch (fd.kind) {
    case FieldKind::F32:
      if (quantized)
        return same_encoded_bits(fd, a, b);
      return same_raw_f32(as_f32(a), as_f32(b));
    case FieldKind::Vec3:
      if (quantized)
        return same_encoded_bits(fd, a, b);
      return same_raw_vec3(as_vec3(a), as_vec3(b));
    case FieldKind::Quat:
      if (quantized)
        return same_encoded_bits(fd, a, b);
      return same_raw_quat(as_quat(a), as_quat(b));
    case FieldKind::I64:
      return as_i64(a) == as_i64(b);
    case FieldKind::Bool:
      return as_bool(a) == as_bool(b);
    case FieldKind::Str:
      return str_ref(a) == str_ref(b);
  }
  return same_encoded_bits(fd, a, b);
}

}  // namespace kcd2s::replication
