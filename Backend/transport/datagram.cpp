#include "kcd2s/transport/datagram.hpp"

namespace kcd2s::transport {

void SimLink::route_to_peer(const Datagram& d) {
  std::uint32_t delay = out_.min_delay;
  if (out_.max_delay > out_.min_delay) {
    std::uniform_int_distribution<std::uint32_t> dd(out_.min_delay, out_.max_delay);
    delay = dd(rng_);
  }
  // Delay is measured in the PEER's poll() steps, so schedule against the peer's clock.
  if (delay == 0) {
    peer_->enqueue_inbound(d);
  } else {
    peer_->enqueue_pending(peer_->now_ + delay, d);
  }
}

void SimLink::send(const std::uint8_t* data, std::size_t len) {
  if (!peer_)
    return;
  Datagram d(data, data + len);

  std::uniform_real_distribution<double> unit(0.0, 1.0);
  if (out_.loss > 0.0 && unit(rng_) < out_.loss)
    return;  // dropped in flight

  route_to_peer(d);
  if (out_.duplicate > 0.0 && unit(rng_) < out_.duplicate)
    route_to_peer(d);  // duplicate copy
}

bool SimLink::recv(Datagram& out) {
  if (inbox_.empty())
    return false;
  out = std::move(inbox_.front());
  inbox_.pop_front();
  return true;
}

void SimLink::poll() {
  ++now_;
  std::vector<Scheduled> still;
  still.reserve(pending_.size());
  for (auto& s : pending_) {
    if (s.release_step <= now_)
      inbox_.push_back(std::move(s.data));
    else
      still.push_back(std::move(s));
  }
  pending_.swap(still);
}

std::pair<std::unique_ptr<SimLink>, std::unique_ptr<SimLink>> make_sim_pair(
    const LinkParams& a_to_b, const LinkParams& b_to_a) {
  auto a = std::make_unique<SimLink>(a_to_b);
  auto b = std::make_unique<SimLink>(b_to_a);
  a->set_peer(b.get());
  b->set_peer(a.get());
  return {std::move(a), std::move(b)};
}

}  // namespace kcd2s::transport
