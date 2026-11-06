// KSTP datagram substrate (test strategy §13).
//
// UDP is the OS primitive; KSTP is everything above it. For headless tests we need an in-process
// datagram link we can drive deterministically, plus a packet-loss/reorder simulator. Real UDP
// sockets implement the same IDatagramLink later without touching the layers above.
#ifndef KCD2S_TRANSPORT_DATAGRAM_HPP
#define KCD2S_TRANSPORT_DATAGRAM_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <utility>
#include <vector>

namespace kcd2s::transport {

using Datagram = std::vector<std::uint8_t>;

// One end of a bidirectional datagram link.
class IDatagramLink {
 public:
  virtual ~IDatagramLink() = default;

  // Queue a datagram toward the peer. Delivery timing depends on the implementation.
  virtual void send(const std::uint8_t* data, std::size_t len) = 0;
  void send(const Datagram& d) {
    send(d.data(), d.size());
  }

  // Pop one received datagram into out; returns false if none is ready.
  virtual bool recv(Datagram& out) = 0;

  // Advance the link's internal clock by one step, maturing delayed datagrams. No-op for links
  // with no delay model.
  virtual void poll() = 0;
};

// Characteristics of one direction of a simulated link.
struct LinkParams {
  double loss = 0.0;            // probability [0,1] a sent datagram is dropped
  double duplicate = 0.0;       // probability [0,1] a delivered datagram is delivered twice
  std::uint32_t min_delay = 0;  // minimum delivery delay, in poll() steps
  std::uint32_t max_delay = 0;  // maximum delivery delay, in poll() steps (>= min_delay)
  std::uint64_t seed = 0;       // RNG seed for deterministic tests
};

// A simulated endpoint. Two of these are wired into a pair; each owns the parameters of its own
// OUTBOUND direction. Datagrams are scheduled into the peer's queues and matured by the peer's
// poll(). With all-zero params this is a lossless, zero-delay loopback.
class SimLink : public IDatagramLink {
 public:
  explicit SimLink(const LinkParams& out_params)
      : out_(out_params), rng_(out_params.seed ? out_params.seed : 0xA5A5A5A5u) {
  }

  using IDatagramLink::send;  // keep the send(const Datagram&) convenience overload visible
  void send(const std::uint8_t* data, std::size_t len) override;
  bool recv(Datagram& out) override;
  void poll() override;

  // Wire this endpoint's outbound side to deliver into `peer`.
  void set_peer(SimLink* peer) {
    peer_ = peer;
  }

 private:
  struct Scheduled {
    std::uint64_t release_step;
    Datagram data;
  };

  void enqueue_inbound(Datagram d) {
    inbox_.push_back(std::move(d));
  }
  void enqueue_pending(std::uint64_t release_step, Datagram d) {
    pending_.push_back(Scheduled{release_step, std::move(d)});
  }
  // Route one (possibly delayed) copy toward the peer, honoring delay.
  void route_to_peer(const Datagram& d);

  LinkParams out_;
  SimLink* peer_ = nullptr;
  std::mt19937_64 rng_;
  std::uint64_t now_ = 0;           // this endpoint's poll-step clock (for inbound maturing)
  std::deque<Datagram> inbox_;      // ready to recv()
  std::vector<Scheduled> pending_;  // delayed, matured by poll()
};

// Create a connected pair of simulated endpoints with per-direction parameters.
//   .first  uses a_to_b for its outbound traffic
//   .second uses b_to_a for its outbound traffic
std::pair<std::unique_ptr<SimLink>, std::unique_ptr<SimLink>> make_sim_pair(
    const LinkParams& a_to_b, const LinkParams& b_to_a);

// Convenience: a lossless, zero-delay connected pair.
inline std::pair<std::unique_ptr<SimLink>, std::unique_ptr<SimLink>> make_loopback_pair() {
  return make_sim_pair(LinkParams{}, LinkParams{});
}

}  // namespace kcd2s::transport

#endif  // KCD2S_TRANSPORT_DATAGRAM_HPP
