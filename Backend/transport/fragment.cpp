#include "kcd2s/transport/fragment.hpp"

#include <utility>

#include "kcd2s/core/bits.hpp"

namespace kcd2s::transport {

using core::BitReader;
using core::BitWriter;

FragmentingLink::FragmentingLink(IDatagramLink* under, std::size_t mtu, FragmentFraming framing)
    : under_(under),
      mtu_(mtu < kFragHeaderBytes + 1 ? kFragHeaderBytes + 1 : mtu),
      framing_(framing) {
}

void FragmentingLink::send_frame(const std::vector<std::uint8_t>& frame) {
  if (frame.size() > largest_frame_)
    largest_frame_ = frame.size();
  under_->send(frame.data(), frame.size());
}

void FragmentingLink::send(const std::uint8_t* data, std::size_t len) {
  if (framing_ == FragmentFraming::Verbatim) {
    const bool needs_escape = len == 0 || is_verbatim_frame_tag(data[0]);
    if (!needs_escape && len <= mtu_) {
      if (len > largest_frame_)
        largest_frame_ = len;
      under_->send(data, len);  // fits and is unambiguous: send it unchanged
      return;
    }
    if (needs_escape && len + kFragTagBytes <= mtu_) {
      std::vector<std::uint8_t> frame;
      frame.reserve(len + kFragTagBytes);
      frame.push_back(kVerbatimEscapeTag);
      frame.insert(frame.end(), data, data + len);
      send_frame(frame);
      return;
    }
  } else if (len + kFragTagBytes <= mtu_) {
    // Small enough to ride whole: a single tag byte then the verbatim payload.
    BitWriter w;
    w.write_bits(0u, 8);  // tag 0 = unfragmented
    if (len)
      w.write_bytes(data, len);
    send_frame(w.bytes());
    return;
  }

  // Fragment: each piece carries at most (mtu - header) payload bytes.
  const std::size_t chunk = mtu_ - kFragHeaderBytes;
  std::size_t frags = (len + chunk - 1) / chunk;
  // Clamp to the wire's fragCount field; messages this large are a bug upstream, but never crash.
  if (frags > kMaxFragments)
    frags = kMaxFragments;
  const std::uint16_t msg_id = next_msg_id_++;
  const std::uint64_t tag = framing_ == FragmentFraming::Verbatim ? kVerbatimFragmentTag : 1u;
  ++fragmented_datagrams_;

  for (std::size_t i = 0; i < frags; ++i) {
    const std::size_t off = i * chunk;
    const std::size_t n = (off + chunk <= len) ? chunk : (len - off);
    BitWriter w;
    w.write_bits(tag, 8);
    w.write_bits(msg_id, 16);
    w.write_bits(static_cast<std::uint64_t>(i), 16);      // frag index
    w.write_bits(static_cast<std::uint64_t>(frags), 16);  // frag count
    w.write_bits(static_cast<std::uint64_t>(len), 32);    // total length
    if (n)
      w.write_bytes(data + off, n);
    send_frame(w.bytes());
    ++fragments_sent_;
  }
}

void FragmentingLink::process(Datagram&& raw) {
  if (raw.empty())
    return;  // garbage; nothing to do

  if (framing_ == FragmentFraming::Verbatim) {
    if (raw[0] == kVerbatimFragmentTag) {
      process_fragment(raw);
    } else if (raw[0] == kVerbatimEscapeTag) {
      ready_.emplace_back(raw.begin() + static_cast<std::ptrdiff_t>(kFragTagBytes), raw.end());
    } else {
      ready_.push_back(std::move(raw));  // unframed whole datagram
    }
    return;
  }

  if (raw[0] == 0) {  // unfragmented: strip the tag, surface the rest
    ready_.emplace_back(raw.begin() + static_cast<std::ptrdiff_t>(kFragTagBytes), raw.end());
    return;
  }
  if (raw[0] != 1)
    return;  // unknown framing -> drop
  process_fragment(raw);
}

void FragmentingLink::process_fragment(const Datagram& raw) {
  BitReader r(raw.data(), raw.size());
  (void)r.read_bits(8);  // tag, already classified by the caller
  const std::uint64_t msg_id = r.read_bits(16);
  const std::uint64_t index = r.read_bits(16);
  const std::uint64_t count = r.read_bits(16);
  const std::uint64_t total = r.read_bits(32);
  if (r.overrun())
    return;

  // Validate against bounds before trusting any of it.
  if (count == 0 || count > kMaxFragments)
    return;
  if (index >= count)
    return;
  if (total > max_message_bytes_)
    return;
  if (raw.size() < kFragHeaderBytes)
    return;

  Partial& p = partial_[static_cast<std::uint16_t>(msg_id)];
  // First fragment seen for this msg establishes the shape; a later fragment that disagrees is a
  // collision/corruption -> reset to the new shape rather than mixing two messages.
  if (p.count == 0) {
    p.count = static_cast<std::uint16_t>(count);
    p.total_len = static_cast<std::uint32_t>(total);
  } else if (p.count != count || p.total_len != total) {
    p = Partial{};
    p.count = static_cast<std::uint16_t>(count);
    p.total_len = static_cast<std::uint32_t>(total);
  }
  p.arrival_step = clock_;
  const std::size_t chunk_bytes = raw.size() - kFragHeaderBytes;
  auto& stored = p.frags[static_cast<std::uint16_t>(index)];
  p.held_bytes = p.held_bytes - stored.size() + chunk_bytes;
  if (p.held_bytes > p.total_len) {
    // More data than the announced total: corrupt or hostile, never buffer past the bound.
    partial_.erase(static_cast<std::uint16_t>(msg_id));
    return;
  }
  stored.assign(raw.begin() + static_cast<std::ptrdiff_t>(kFragHeaderBytes), raw.end());

  // Capacity (newer-msg) eviction: too many partials in flight -> drop the oldest-arrival one.
  // Never evict the one we just touched.
  while (partial_.size() > max_partials_) {
    const std::size_t before = partial_.size();
    // Find oldest excluding the current msg.
    auto oldest = partial_.end();
    for (auto it = partial_.begin(); it != partial_.end(); ++it) {
      if (it->first == static_cast<std::uint16_t>(msg_id))
        continue;
      if (oldest == partial_.end() || it->second.arrival_step < oldest->second.arrival_step)
        oldest = it;
    }
    if (oldest == partial_.end())
      break;
    partial_.erase(oldest);
    if (partial_.size() == before)
      break;  // safety
  }

  // Complete? Concatenate in index order and verify the reassembled size.
  auto it = partial_.find(static_cast<std::uint16_t>(msg_id));
  if (it == partial_.end())
    return;
  Partial& cur = it->second;
  if (cur.frags.size() != cur.count)
    return;  // still waiting on pieces

  Datagram assembled;
  assembled.reserve(cur.total_len);
  for (std::uint16_t i = 0; i < cur.count; ++i) {
    auto f = cur.frags.find(i);
    if (f == cur.frags.end()) {  // a gap despite the count match (shouldn't happen) -> bail
      partial_.erase(it);
      return;
    }
    assembled.insert(assembled.end(), f->second.begin(), f->second.end());
  }
  const bool ok = assembled.size() == cur.total_len;
  partial_.erase(it);
  if (ok)
    ready_.push_back(std::move(assembled));
}

bool FragmentingLink::recv(Datagram& out) {
  for (;;) {
    if (!ready_.empty()) {
      out = std::move(ready_.front());
      ready_.pop_front();
      return true;
    }
    Datagram raw;
    if (!under_->recv(raw))
      return false;  // nothing left to pull
    process(std::move(raw));
  }
}

void FragmentingLink::poll() {
  under_->poll();
  ++clock_;
  if (timeout_ == 0)
    return;
  for (auto it = partial_.begin(); it != partial_.end();) {
    if (clock_ - it->second.arrival_step > timeout_)
      it = partial_.erase(it);
    else
      ++it;
  }
}

}  // namespace kcd2s::transport
