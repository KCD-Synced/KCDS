#include "kcd2s/transport/endpoint.hpp"

#include <algorithm>

namespace kcd2s::transport {

bool ServerEndpoint::rate_ok(std::uint64_t addr, double now) {
  auto it = buckets_.find(addr);
  if (it == buckets_.end()) {
    // Evict the most-idle bucket if the table is at capacity.
    if (buckets_.size() >= rl_max_buckets_) {
      auto oldest = buckets_.begin();
      for (auto b = buckets_.begin(); b != buckets_.end(); ++b)
        if (b->second.last_used < oldest->second.last_used)
          oldest = b;
      buckets_.erase(oldest);
    }
    it = buckets_.emplace(addr, Bucket{rl_burst_, now, now}).first;  // new source starts full
  }
  Bucket& b = it->second;
  b.tokens = std::min(rl_burst_, b.tokens + (now - b.last) * rl_rate_);  // refill
  b.last = now;
  b.last_used = now;
  if (b.tokens < 1.0)
    return false;
  b.tokens -= 1.0;
  return true;
}

std::vector<DeliveredMessage> ServerEndpoint::feed(std::uint64_t addr, const Datagram& dg,
                                                   double now) {
  // Flood gate first: an over-budget source is dropped before any handshake/gate work.
  if (rl_rate_ > 0.0 && !rate_ok(addr, now)) {
    ++rl_dropped_;
    return {};
  }

  auto it = peers_.find(addr);
  if (it != peers_.end()) {
    // Admitted peer: route the datagram into its Connection and return what it delivers.
    it->second.link->deliver(dg);
    return it->second.conn->receive(now);
  }

  // Un-admitted: advance the handshake. A non-handshake datagram makes on_datagram return false and
  // admits nothing (ignored).
  std::vector<std::uint8_t> reply;
  if (gate_.on_datagram(addr, dg, reply) && send_)
    send_(addr, reply);

  // If that datagram was the valid CHALLENGE_RESPONSE, the gate now lists the address as admitted;
  // stand up its Connection once.
  if (gate_.admitted(addr) && peers_.find(addr) == peers_.end()) {
    Peer p;
    p.link = std::make_unique<PeerLink>(addr, &send_);
    p.conn = std::make_unique<Connection>(p.link.get());
    p.admitted_at = now;          // liveness grace until the first channel datagram arrives
    p.session = next_session_++;  // fresh logical identity (a resume may rebind it)
    Connection& conn_ref = *p.conn;
    peers_.emplace(addr, std::move(p));
    if (on_admit_)
      on_admit_(addr, conn_ref);
  }
  return {};  // handshake datagrams carry no channel messages
}

std::size_t ServerEndpoint::reap_timed_out(double now, double threshold) {
  std::vector<std::uint64_t> dead;
  for (auto& [addr, p] : peers_) {
    // Liveness reference: the last received datagram, or the admit time if none yet.
    const double ref = p.conn->has_received_datagram() ? p.conn->last_recv_time() : p.admitted_at;
    if (now - ref > threshold)
      dead.push_back(addr);
  }
  for (std::uint64_t addr : dead) {
    auto it = peers_.find(addr);
    if (it != peers_.end())
      grace_[it->second.session] = now;  // resumable within the window
    if (on_drop_)
      on_drop_(addr);
    gate_.forget(addr);    // allow a fresh handshake from this address
    buckets_.erase(addr);  // its rate bucket is no longer needed
    peers_.erase(addr);
  }
  return dead.size();
}

ServerEndpoint::SessionId ServerEndpoint::session_id(std::uint64_t addr) const {
  auto it = peers_.find(addr);
  return it == peers_.end() ? 0u : it->second.session;
}

std::uint64_t ServerEndpoint::resume_token(std::uint64_t addr) const {
  auto it = peers_.find(addr);
  if (it == peers_.end())
    return 0u;
  return gate_.session_token(it->second.session);
}

ServerEndpoint::ResumeOutcome ServerEndpoint::try_resume(std::uint64_t addr, SessionId old_session,
                                                         std::uint64_t old_token, double now) {
  ResumeOutcome r;
  auto it = peers_.find(addr);
  if (it == peers_.end())
    return r;  // must be admitted (fresh handshake) first
  r.session = it->second.session;

  // Prune expired grace entries so the table can't grow unbounded.
  for (auto g = grace_.begin(); g != grace_.end();) {
    if (now - g->second > resume_grace_)
      g = grace_.erase(g);
    else
      ++g;
  }

  // Verify the bearer token binds this session under the server secret.
  if (gate_.session_token(old_session) != old_token)
    return r;  // forged/invalid -> stay fresh

  auto gi = grace_.find(old_session);
  if (gi == grace_.end() || now - gi->second > resume_grace_)
    return r;  // expired/unknown -> fresh

  // Rebind to the prior identity.
  it->second.session = old_session;
  grace_.erase(gi);
  r.resumed = true;
  r.session = old_session;
  return r;
}

void ServerEndpoint::flush(double now) {
  for (auto& [addr, p] : peers_) {
    (void)addr;
    p.conn->flush(now);
  }
}

Connection* ServerEndpoint::connection(std::uint64_t addr) {
  auto it = peers_.find(addr);
  return it == peers_.end() ? nullptr : it->second.conn.get();
}

}  // namespace kcd2s::transport
