// Shared integration constants for the headless server/client peers.
#ifndef KCD2S_INTEGRATION_PROTOCOL_HPP
#define KCD2S_INTEGRATION_PROTOCOL_HPP

#include <cstdint>
#include <vector>

#include "kcd2s/core/ids.hpp"

namespace kcd2s::integration {

// Logical channel the client uses to report the newest snapshot tick it has applied, so the server
// can advance that client's baseline (baseline ack). UnreliableSequenced =
// keep-newest, which is exactly the semantics a monotonic baseline wants. Beyond the §2 channel 0-4
// range on purpose.
constexpr std::uint8_t kChannelReplAck = 5;

inline std::vector<std::uint8_t> encode_tick(core::Tick t) {
  return {static_cast<std::uint8_t>(t & 0xFF), static_cast<std::uint8_t>((t >> 8) & 0xFF),
          static_cast<std::uint8_t>((t >> 16) & 0xFF), static_cast<std::uint8_t>((t >> 24) & 0xFF)};
}

// What a client can do beyond the first protocol, said on channel 0 with each world-ready
// (kClientFeaturesTag, 'C', 'F', feature bits). A server without this ignores the message; a
// client that does not send it is served as before.
constexpr std::uint8_t kClientFeaturesTag = 177;
// It acknowledges every snapshot it applies, not only the newest of each of its frames: the
// server sends an entity's CONSTRUCT once and again only when its acknowledgement is overdue,
// instead of in every snapshot until one that carried it is acknowledged.
constexpr std::uint8_t kFeatureAcksEverySnapshot = 1;
// It reads snapshots on kCompactSnapshotChannel, whose UPDATEs name their changed fields in the
// compact form (replication::MaskForm::Compact): the same values in fewer bytes.
constexpr std::uint8_t kFeatureCompactSnapshots = 2;
constexpr std::uint8_t kClientFeatures = kFeatureAcksEverySnapshot | kFeatureCompactSnapshots;
// Snapshots with compact field masks, for clients that said they read them.
constexpr std::uint8_t kCompactSnapshotChannel = 11;

inline std::vector<std::uint8_t> encode_client_features(std::uint8_t features) {
  return {kClientFeaturesTag, 'C', 'F', features};
}
inline bool decode_client_features(const std::vector<std::uint8_t>& p, std::uint8_t& features) {
  if (p.size() != 4 || p[0] != kClientFeaturesTag || p[1] != 'C' || p[2] != 'F') {
    return false;
  }
  features = p[3];
  return true;
}

inline core::Tick decode_tick(const std::vector<std::uint8_t>& b) {
  if (b.size() < 4)
    return 0;
  return static_cast<core::Tick>(b[0]) | (static_cast<core::Tick>(b[1]) << 8) |
         (static_cast<core::Tick>(b[2]) << 16) | (static_cast<core::Tick>(b[3]) << 24);
}

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_PROTOCOL_HPP
