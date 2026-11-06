#include "kcd2s/sdk/marshal.hpp"

#include <cstring>

#include "kcd2s/core/bits.hpp"

namespace kcd2s::sdk {

using core::BitReader;
using core::BitWriter;

namespace {

void put_tag(BitWriter& w, Tag t) {
  w.write_bits(static_cast<std::uint64_t>(t), 8);
}

void put_str(BitWriter& w, const std::string& s) {
  w.write_varuint(s.size());
  w.write_byte_run(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

void encode_into(BitWriter& w, const Value& v) {
  const Tag t = v.tag();
  put_tag(w, t);
  switch (t) {
    case Tag::Null:
      break;
    case Tag::Bool:
      w.write_bits(v.as_bool() ? 1u : 0u, 8);
      break;
    case Tag::Int:
      w.write_varint(v.as_int());
      break;
    case Tag::Double: {
      std::uint64_t bits;
      double d = v.as_double();
      std::memcpy(&bits, &d, 8);
      w.write_bits(bits, 64);
      break;
    }
    case Tag::Str:
      put_str(w, v.as_str());
      break;
    case Tag::Bytes: {
      const auto& b = v.as_bytes();
      w.write_varuint(b.size());
      w.write_byte_run(b.data(), b.size());
      break;
    }
    case Tag::Array: {
      const auto& a = v.as_array();
      w.write_varuint(a.size());
      for (const Value& e : a)
        encode_into(w, e);
      break;
    }
    case Tag::Map: {
      const auto& m = v.as_map();
      w.write_varuint(m.size());
      for (const auto& [k, val] : m) {
        put_str(w, k);
        encode_into(w, val);
      }
      break;
    }
    case Tag::EntityRef:
      w.write_varuint(v.as_entity().id);
      break;
    case Tag::FuncRef:
      w.write_varuint(v.as_func().resource);
      w.write_varuint(v.as_func().id);
      break;
  }
}

// Recursive decode with a guard against runaway nesting of malformed input.
bool decode_into(BitReader& r, Value& out, int depth) {
  if (depth > 64)
    return false;
  const std::uint8_t tag_byte = static_cast<std::uint8_t>(r.read_bits(8));
  if (r.overrun())
    return false;
  if (tag_byte > static_cast<std::uint8_t>(Tag::FuncRef))
    return false;
  const Tag t = static_cast<Tag>(tag_byte);

  auto read_str = [&](std::string& s) -> bool {
    const std::uint64_t len = r.read_varuint();
    if (r.overrun())
      return false;
    if (len > r.bits_remaining() / 8)
      return false;  // division avoids len*8 overflow for huge len
    s.resize(static_cast<std::size_t>(len));
    r.read_byte_run(reinterpret_cast<std::uint8_t*>(s.data()), s.size());
    return !r.overrun();
  };

  switch (t) {
    case Tag::Null:
      out = Value::null();
      return true;
    case Tag::Bool:
      out = Value(r.read_bits(8) != 0);
      return !r.overrun();
    case Tag::Int:
      out = Value(r.read_varint());
      return !r.overrun();
    case Tag::Double: {
      const std::uint64_t bits = r.read_bits(64);
      if (r.overrun())
        return false;
      double d;
      std::memcpy(&d, &bits, 8);
      out = Value(d);
      return true;
    }
    case Tag::Str: {
      std::string s;
      if (!read_str(s))
        return false;
      out = Value(std::move(s));
      return true;
    }
    case Tag::Bytes: {
      const std::uint64_t len = r.read_varuint();
      if (r.overrun() || len > r.bits_remaining() / 8)
        return false;  // avoid len*8 overflow
      Value::Bytes b(static_cast<std::size_t>(len));
      r.read_byte_run(b.data(), b.size());
      if (r.overrun())
        return false;
      out = Value::bytes(std::move(b));
      return true;
    }
    case Tag::Array: {
      const std::uint64_t n = r.read_varuint();
      if (r.overrun() || n > r.bits_remaining())
        return false;  // each element >= 1 byte
      Value::Array a;
      a.reserve(static_cast<std::size_t>(n));
      for (std::uint64_t i = 0; i < n; ++i) {
        Value e;
        if (!decode_into(r, e, depth + 1))
          return false;
        a.push_back(std::move(e));
      }
      out = Value(std::move(a));
      return true;
    }
    case Tag::Map: {
      const std::uint64_t n = r.read_varuint();
      if (r.overrun() || n > r.bits_remaining())
        return false;
      Value::Map m;
      m.reserve(static_cast<std::size_t>(n));
      for (std::uint64_t i = 0; i < n; ++i) {
        std::string k;
        if (!read_str(k))
          return false;
        Value val;
        if (!decode_into(r, val, depth + 1))
          return false;
        m.emplace_back(std::move(k), std::move(val));
      }
      out = Value(std::move(m));
      return true;
    }
    case Tag::EntityRef: {
      const std::uint64_t id = r.read_varuint();
      if (r.overrun())
        return false;
      out = Value(EntityRef{static_cast<core::EntityId>(id)});
      return true;
    }
    case Tag::FuncRef: {
      const std::uint64_t res = r.read_varuint();
      const std::uint64_t id = r.read_varuint();
      if (r.overrun())
        return false;
      out = Value(FuncRef{static_cast<std::uint32_t>(res), static_cast<std::uint32_t>(id)});
      return true;
    }
  }
  return false;
}

}  // namespace

std::vector<std::uint8_t> encode(const Value& v) {
  BitWriter w;
  encode_into(w, v);
  return w.bytes();
}

std::vector<std::uint8_t> encode_args(const Value::Array& args) {
  return encode(Value(args));
}

DecodeResult decode(const std::uint8_t* data, std::size_t len) {
  BitReader r(data, len);
  DecodeResult res;
  res.ok = decode_into(r, res.value, 0);
  res.consumed = res.ok ? (r.bits_read() + 7) / 8 : 0;
  return res;
}

}  // namespace kcd2s::sdk
