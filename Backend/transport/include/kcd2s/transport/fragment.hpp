// KSTP MTU fragmentation + reassembly.
//
// UDP datagrams larger than the path MTU get fragmented by the IP layer (or dropped outright with
// DF set), and a single lost IP fragment silently discards the whole datagram with no signal we can
// act on. KSTP therefore fragments at its OWN layer, just above the datagram substrate, so that:
//   - large snapshot payloads are split into MTU-sized pieces we control,
//   - a lost piece is a lost KSTP fragment (the reassembly simply never completes and is dropped),
//   - the Connection/ack machinery above never sees any of this — it hands whole datagrams down and
//     gets whole datagrams back.
//
// FragmentingLink is a transparent IDatagramLink wrapper with two framings.
//
// Tagged (the default) prefixes every outbound datagram with a tag byte:
//   tag 0x00  unfragmented : [u8 tag=0][payload...]                       (len+1 <= mtu)
//   tag 0x01  fragment     : [u8 tag=1][u16 msgId][u16 fragIndex]
//                            [u16 fragCount][u32 totalLen][payload chunk]
//
// Verbatim (used by the KSTP UDP host and client) sends a datagram that fits unchanged
// and adds framing only where it is needed. Two leading byte values are reserved for it; a KSTP
// packet starts with the low byte of its protocol id and a handshake frame with its kind (1-5),
// so neither ever collides with them:
//   whole          : [payload...]                 payload fits, does not start with 0xFD/0xFE
//   escaped whole  : [u8 0xFD][payload...]        payload empty or starts with 0xFD/0xFE
//   fragment       : [u8 0xFE][u16 msgId][u16 fragIndex][u16 fragCount][u32 totalLen][chunk]
// The receiver collects fragments by msgId until all `fragCount` pieces are present, concatenates
// them in index order, verifies the total length, and surfaces the original datagram. Incomplete
// sets are dropped on a poll-step timeout or when too many partials pile up (newer-msg eviction),
// so a lossy link can never leak memory or stall reassembly.
#ifndef KCD2S_TRANSPORT_FRAGMENT_HPP
#define KCD2S_TRANSPORT_FRAGMENT_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include "kcd2s/transport/datagram.hpp"

namespace kcd2s::transport {

// Fragment framing constants (bytes).
inline constexpr std::size_t kFragTagBytes = 1;      // leading tag byte on every datagram
inline constexpr std::size_t kFragHeaderBytes = 11;  // tag + msgId + fragIndex + fragCount + total
inline constexpr std::size_t kDefaultMtu = 1200;     // conservative UDP payload (avoids IP frag)

// Safety bounds so a hostile/garbled header can never trigger a huge allocation.
inline constexpr std::uint16_t kMaxFragments = 1024;       // per message
inline constexpr std::uint32_t kMaxReassembly = 4u << 20;  // 4 MiB per message

// Reserved leading bytes of the verbatim framing.
inline constexpr std::uint8_t kVerbatimEscapeTag = 0xFD;
inline constexpr std::uint8_t kVerbatimFragmentTag = 0xFE;

enum class FragmentFraming : std::uint8_t {
  Tagged,    // every datagram carries a tag byte
  Verbatim,  // datagrams that fit are sent unchanged; see the header comment
};

// True when `first_byte` starts a verbatim-framing frame (escaped whole datagram or fragment).
inline bool is_verbatim_frame_tag(std::uint8_t first_byte) {
  return first_byte == kVerbatimEscapeTag || first_byte == kVerbatimFragmentTag;
}

// Transparent fragmentation/reassembly layer sitting between a Connection and the real link.
class FragmentingLink : public IDatagramLink {
 public:
  // `under` is the underlying link (SimLink in tests, a UDP socket later); not owned. `mtu` is the
  // max size of a datagram this layer will emit to `under` (must exceed the fragment header).
  explicit FragmentingLink(IDatagramLink* under, std::size_t mtu = kDefaultMtu,
                           FragmentFraming framing = FragmentFraming::Tagged);

  using IDatagramLink::send;  // keep the send(const Datagram&) convenience overload visible
  void send(const std::uint8_t* data, std::size_t len) override;
  bool recv(Datagram& out) override;
  void poll() override;

  // Drop a partial reassembly this many poll() steps after its latest fragment arrived (0 disables
  // the time-based sweep; capacity eviction still applies).
  void set_reassembly_timeout(std::uint32_t polls) {
    timeout_ = polls;
  }
  // Cap the number of in-flight partial reassemblies; the oldest is evicted past this.
  void set_max_partials(std::size_t n) {
    max_partials_ = n ? n : 1;
  }
  // Reject fragment sets that announce a reassembled size above `bytes` (at most kMaxReassembly).
  void set_max_message_bytes(std::uint32_t bytes) {
    max_message_bytes_ = bytes < kMaxReassembly ? bytes : kMaxReassembly;
  }

  // Introspection for tests.
  std::size_t partial_count() const {
    return partial_.size();
  }
  std::size_t mtu() const {
    return mtu_;
  }
  std::uint64_t fragments_sent() const {
    return fragments_sent_;
  }
  // Outbound datagrams that had to be split into fragments.
  std::uint64_t fragmented_datagrams() const {
    return fragmented_datagrams_;
  }
  // Largest datagram handed to the underlying link so far (never above mtu()).
  std::size_t largest_frame() const {
    return largest_frame_;
  }
  FragmentFraming framing() const {
    return framing_;
  }

 private:
  struct Partial {
    std::uint16_t count = 0;
    std::uint32_t total_len = 0;
    std::uint64_t arrival_step = 0;  // poll-step clock at the latest received fragment
    std::size_t held_bytes = 0;      // sum of the stored chunk sizes
    std::map<std::uint16_t, std::vector<std::uint8_t>> frags;  // index -> chunk (dedups by index)
  };

  // Parse one raw datagram pulled from `under_`, pushing any completed/unfragmented datagram onto
  // ready_.
  void process(Datagram&& raw);
  void process_fragment(const Datagram& raw);
  void send_frame(const std::vector<std::uint8_t>& frame);

  IDatagramLink* under_;
  std::size_t mtu_;
  FragmentFraming framing_;
  std::uint32_t max_message_bytes_ = kMaxReassembly;
  std::uint16_t next_msg_id_ = 0;
  std::uint64_t clock_ = 0;      // local poll-step clock for timeouts
  std::uint32_t timeout_ = 600;  // ~ generous; tests override
  std::size_t max_partials_ = 64;

  std::unordered_map<std::uint16_t, Partial> partial_;
  std::deque<Datagram> ready_;  // reassembled or unfragmented datagrams awaiting recv()

  std::uint64_t fragments_sent_ = 0;
  std::uint64_t fragmented_datagrams_ = 0;
  std::size_t largest_frame_ = 0;
};

}  // namespace kcd2s::transport

#endif  // KCD2S_TRANSPORT_FRAGMENT_HPP
