#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>
#include "kcd2s/core/bits.hpp"
#include "kcd2s/core/ids.hpp"
#include "kcd2s/sdk/marshal.hpp"
#include "kcd2s/transport/connection.hpp"

namespace kcd2s::integration {

inline constexpr std::uint8_t kScriptChannel = 2;
inline constexpr std::size_t kScriptMessageLimit = 65536;
inline constexpr std::size_t kScriptFragmentBytes = 850;

inline const sdk::Value* script_field(const sdk::Value& value, const char* name) {
  if (value.tag() != sdk::Tag::Map)
    return nullptr;
  for (const auto& entry : value.as_map()) {
    if (entry.first == name)
      return &entry.second;
  }
  return nullptr;
}

inline std::string script_operation(const sdk::Value& value) {
  if (value.tag() != sdk::Tag::Map)
    return {};
  std::size_t count = 0;
  const sdk::Value* op = nullptr;
  for (const auto& entry : value.as_map()) {
    if (entry.first == "op") {
      ++count;
      op = &entry.second;
    }
  }
  return count == 1 && op && op->tag() == sdk::Tag::Str ? op->as_str() : std::string{};
}

// The encoded message, or empty when it cannot be sent (no single string "op", or too large).
// Encode once and queue the bytes on every connection that gets the message.
inline std::vector<std::uint8_t> encode_script(const sdk::Value& value) {
  if (script_operation(value).empty())
    return {};
  auto bytes = sdk::encode(value);
  if (bytes.size() > kScriptMessageLimit)
    return {};
  return bytes;
}

// Queue an encoded message (encode_script) on channel 2, in fragments that carry this
// connection's world revision and next message sequence.
inline bool queue_script_bytes(transport::Connection& connection, std::uint32_t revision,
                               std::uint32_t& next_sequence,
                               const std::vector<std::uint8_t>& bytes) {
  if (!revision || bytes.empty())
    return false;
  if (++next_sequence == 0)
    ++next_sequence;
  const auto count = (bytes.size() + kScriptFragmentBytes - 1) / kScriptFragmentBytes;
  for (std::size_t index = 0; index < count; ++index) {
    const auto offset = index * kScriptFragmentBytes;
    const auto length = std::min(kScriptFragmentBytes, bytes.size() - offset);
    core::BitWriter writer;
    writer.write_bits(0x3152434b, 32);
    writer.write_bits(revision, 32);
    writer.write_bits(next_sequence, 32);
    writer.write_bits(index, 16);
    writer.write_bits(count, 16);
    writer.write_bits(bytes.size(), 32);
    writer.write_bytes(bytes.data() + offset, length);
    connection.queue(kScriptChannel, writer.bytes());
  }
  return true;
}

inline bool queue_script(transport::Connection& connection, std::uint32_t revision,
                         std::uint32_t& next_sequence, const sdk::Value& value) {
  return queue_script_bytes(connection, revision, next_sequence, encode_script(value));
}

// Channel 2 is reliable ordered. One bounded assembly is sufficient: a new
// message cannot overtake the unfinished preceding message on this channel.
class ScriptReceiver {
 public:
  std::optional<sdk::Value> receive(const std::vector<std::uint8_t>& frame,
                                    std::uint32_t revision) {
    core::BitReader reader(frame);
    if (reader.read_bits(32) != 0x3152434b || reader.read_bits(32) != revision || !revision)
      return std::nullopt;
    const auto sequence = std::uint32_t(reader.read_bits(32));
    const auto index = std::size_t(reader.read_bits(16));
    const auto count = std::size_t(reader.read_bits(16));
    const auto total = std::size_t(reader.read_bits(32));
    const auto expected_count = (total + kScriptFragmentBytes - 1) / kScriptFragmentBytes;
    if (reader.overrun() || !sequence || !total || total > kScriptMessageLimit ||
        count != expected_count || index >= count)
      return std::nullopt;
    const auto expected = std::min(kScriptFragmentBytes, total - index * kScriptFragmentBytes);
    if (reader.bits_remaining() != expected * 8)
      return std::nullopt;
    if (index == 0) {
      bytes_.clear();
      bytes_.reserve(total);
      sequence_ = sequence;
      revision_ = revision;
      total_ = total;
      next_index_ = 0;
    }
    if (sequence_ != sequence || revision_ != revision || total_ != total || next_index_ != index)
      return std::nullopt;
    for (std::size_t i = 0; i < expected; ++i)
      bytes_.push_back(std::uint8_t(reader.read_bits(8)));
    ++next_index_;
    if (bytes_.size() != total)
      return std::nullopt;
    auto result = sdk::decode(bytes_);
    bytes_.clear();
    sequence_ = 0;
    if (!result.ok || result.consumed != total || script_operation(result.value).empty())
      return std::nullopt;
    return std::move(result.value);
  }

  void reset() {
    bytes_.clear();
    sequence_ = 0;
    revision_ = 0;
    total_ = 0;
    next_index_ = 0;
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::uint32_t sequence_ = 0;
  std::uint32_t revision_ = 0;
  std::size_t total_ = 0;
  std::size_t next_index_ = 0;
};

struct ScriptSubmission {
  core::PeerId peer{};
  core::EntityId player{};
  sdk::Value message;
};

}  // namespace kcd2s::integration
