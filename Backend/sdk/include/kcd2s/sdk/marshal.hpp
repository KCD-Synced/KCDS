// Network marshaling for the canonical Value (§6). A compact tagged,
// self-describing byte format so client<->server events/RPC are language-independent. The stream is
// byte-aligned (tag byte + varuint lengths + payloads), so it is portable and easy to reimplement
// in each runtime's native marshaler.
#ifndef KCD2S_SDK_MARSHAL_HPP
#define KCD2S_SDK_MARSHAL_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcd2s::sdk {

// Serialize one Value (or a list, as an Array) to bytes.
std::vector<std::uint8_t> encode(const Value& v);
std::vector<std::uint8_t> encode_args(const Value::Array& args);  // convenience for event/RPC args

// Parse one Value. Returns false (ok=false) on malformed/truncated input; treat all input as
// untrusted network data. On success `out` holds the value and `consumed` the bytes read.
struct DecodeResult {
  bool ok = false;
  Value value;
  std::size_t consumed = 0;
};
DecodeResult decode(const std::uint8_t* data, std::size_t len);
inline DecodeResult decode(const std::vector<std::uint8_t>& buf) {
  return decode(buf.data(), buf.size());
}

}  // namespace kcd2s::sdk

#endif  // KCD2S_SDK_MARSHAL_HPP
