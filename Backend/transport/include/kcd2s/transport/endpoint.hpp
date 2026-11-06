// ServerEndpoint — the connectionless front of a KSTP server.
//
// A real UDP server has one socket receiving datagrams from many sources; recvfrom yields
// (srcAddr, datagram). ServerEndpoint sits on that: it runs the stateless cookie handshake
// (ConnectionGate) per source address and only AFTER a source is admitted does it create a per-peer
// Connection for it. Channel traffic from an un-admitted address is ignored, so no Connection (and
// no per-peer state beyond the gate's admitted set) exists for a source that has not proven itself.
//
// It owns no socket: `feed(addr, datagram, now)` is called with each received datagram, and
// outbound datagrams go through an injected SendFn(addr, datagram) (the socket sendto). This keeps
// the whole thing headless-testable and leaves Connection + ConnectionGate untouched. The per-peer
// Connection is reused verbatim via a tiny IDatagramLink adapter that routes its sends to
// SendFn(addr, ...) and its receives from datagrams feed() hands it.
#ifndef KCD2S_TRANSPORT_ENDPOINT_HPP
#define KCD2S_TRANSPORT_ENDPOINT_HPP

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/transport/handshake.hpp"

namespace kcd2s::transport {

class ServerEndpoint {
 public:
  // A stable logical session identity, preserved across a reconnect via a resume token so the
  // integration layer can rebind the same player/avatar.
  using SessionId = std::uint64_t;

  // How to send a datagram back to a source address (the socket sendto). Set before use.
  using SendFn = std::function<void(std::uint64_t addr, const Datagram& dg)>;
  // Invoked once when an address is admitted, exposing its fresh Connection.
  using AdmitFn = std::function<void(std::uint64_t addr, Connection& conn)>;
  // Invoked when a peer is dropped (timeout reap), before its Connection is destroyed.
  using DropFn = std::function<void(std::uint64_t addr)>;

  explicit ServerEndpoint(std::uint64_t secret) : gate_(secret) {
  }

  void set_send(SendFn fn) {
    send_ = std::move(fn);
  }
  void set_on_admit(AdmitFn fn) {
    on_admit_ = std::move(fn);
  }
  void set_on_drop(DropFn fn) {
    on_drop_ = std::move(fn);
  }

  // Process one received datagram from `addr`. For an admitted peer the datagram is routed into its
  // Connection and the delivered channel messages are returned. For an un-admitted source the
  // handshake is advanced (any reply is sent via SendFn) and {} is returned; a non-handshake
  // datagram from an un-admitted source is ignored.
  std::vector<DeliveredMessage> feed(std::uint64_t addr, const Datagram& dg, double now);

  // Flush every admitted peer's Connection (sends via SendFn).
  void flush(double now);

  // Drop peers whose Connection has been silent for more than `threshold` seconds (no datagram
  // received), firing on_drop(addr) for each and freeing its Connection — a crashed/vanished client
  // is cleaned up without a graceful leave. A reaped address may handshake again from scratch.
  // Returns the number reaped. (default threshold ~10s)
  std::size_t reap_timed_out(double now, double threshold = 10.0);

  // Flood protection (platform §6): cap inbound datagrams per source address with a token bucket,
  // checked before the handshake/gate so a spoofed/abusive source cannot flood cookie challenges or
  // a peer's channel. `per_sec` tokens refill per second into a `burst`-deep bucket; `per_sec <= 0`
  // disables limiting (the default). `max_buckets` bounds the per-address table (idle buckets are
  // evicted) so the limiter itself can't exhaust memory.
  void set_rate_limit(double per_sec, double burst, std::size_t max_buckets = 4096) {
    rl_rate_ = per_sec;
    rl_burst_ = burst;
    rl_max_buckets_ = max_buckets ? max_buckets : 1;
  }
  std::uint64_t rate_limited_dropped() const {
    return rl_dropped_;
  }
  std::size_t rate_bucket_count() const {
    return buckets_.size();
  }

  // --- reconnect / resume (session continuity across a transient drop) ---
  // The logical session id assigned to an admitted address (0 if not admitted).
  SessionId session_id(std::uint64_t addr) const;
  // The resume token the owner of `addr` should keep and present after a drop (0 if not admitted).
  std::uint64_t resume_token(std::uint64_t addr) const;
  // How long a dropped session stays resumable (seconds).
  void set_resume_grace(double seconds) {
    resume_grace_ = seconds;
  }

  struct ResumeOutcome {
    bool resumed = false;   // true if the old session was rebound
    SessionId session = 0;  // the session id now in effect for the address
  };
  // An already-admitted `addr` (via a fresh handshake) presents a prior (old_session, old_token).
  // If the token verifies AND the session was dropped within the grace window, the address's
  // session is rebound to old_session (resumed=true). An invalid/forged token or an expired/unknown
  // session leaves the address on its fresh session (resumed=false). No-op for an un-admitted addr.
  ResumeOutcome try_resume(std::uint64_t addr, SessionId old_session, std::uint64_t old_token,
                           double now);

  bool admitted(std::uint64_t addr) const {
    return peers_.count(addr) != 0;
  }
  std::size_t peer_count() const {
    return peers_.size();
  }
  Connection* connection(std::uint64_t addr);

 private:
  // Per-peer IDatagramLink: sends via the endpoint's SendFn (bound to this addr), receives from an
  // inbox the endpoint fills from feed().
  class PeerLink : public IDatagramLink {
   public:
    PeerLink(std::uint64_t addr, SendFn* send) : addr_(addr), send_(send) {
    }
    using IDatagramLink::send;
    void send(const std::uint8_t* data, std::size_t len) override {
      if (*send_)
        (*send_)(addr_, Datagram(data, data + len));
    }
    bool recv(Datagram& out) override {
      if (inbox_.empty())
        return false;
      out = std::move(inbox_.front());
      inbox_.pop_front();
      return true;
    }
    void poll() override {
    }
    void deliver(const Datagram& dg) {
      inbox_.push_back(dg);
    }

   private:
    std::uint64_t addr_;
    SendFn* send_;
    std::deque<Datagram> inbox_;
  };

  struct Peer {
    std::unique_ptr<PeerLink> link;
    std::unique_ptr<Connection> conn;
    double admitted_at = 0.0;  // grace basis before the first datagram is received
    SessionId session = 0;     // logical identity (preserved across a resume)
  };

  // Token bucket per source address.
  struct Bucket {
    double tokens = 0.0;
    double last = 0.0;       // last refill time
    double last_used = 0.0;  // for idle eviction
  };
  bool rate_ok(std::uint64_t addr, double now);  // false => over budget (drop)

  ConnectionGate gate_;
  SendFn send_;
  AdmitFn on_admit_;
  DropFn on_drop_;
  std::unordered_map<std::uint64_t, Peer> peers_;

  double rl_rate_ = 0.0;  // tokens/sec; <=0 disables
  double rl_burst_ = 0.0;
  std::size_t rl_max_buckets_ = 4096;
  std::uint64_t rl_dropped_ = 0;
  std::unordered_map<std::uint64_t, Bucket> buckets_;

  // Resume: fresh sessions get a monotonically increasing id; dropped sessions linger in `grace_`
  // (session -> drop time) until the resume window expires.
  SessionId next_session_ = 1;
  double resume_grace_ = 30.0;
  std::unordered_map<SessionId, double> grace_;
};

}  // namespace kcd2s::transport

#endif  // KCD2S_TRANSPORT_ENDPOINT_HPP
