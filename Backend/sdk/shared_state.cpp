#include "kcd2s/sdk/shared_state.hpp"

#include "kcd2s/sdk/marshal.hpp"

namespace kcd2s::sdk {

namespace {

// Byte-level LEB128 (the delta stream is byte-aligned throughout, so marshalled Values embed
// cleanly between keys).
void put_varuint(std::vector<std::uint8_t>& out, std::uint64_t v) {
  do {
    std::uint8_t group = static_cast<std::uint8_t>(v & 0x7Fu);
    v >>= 7;
    if (v)
      group |= 0x80u;
    out.push_back(group);
  } while (v);
}

bool get_varuint(const std::uint8_t* data, std::size_t len, std::size_t& pos, std::uint64_t& out) {
  std::uint64_t v = 0;
  int shift = 0;
  while (shift < 64) {
    if (pos >= len)
      return false;
    const std::uint8_t group = data[pos++];
    v |= static_cast<std::uint64_t>(group & 0x7Fu) << shift;
    if ((group & 0x80u) == 0) {
      out = v;
      return true;
    }
    shift += 7;
  }
  return false;  // varuint longer than 64 bits -> malformed
}

void put_key(std::vector<std::uint8_t>& out, const std::string& k) {
  put_varuint(out, k.size());
  out.insert(out.end(), k.begin(), k.end());
}

}  // namespace

bool SharedState::set(const std::string& key, Value v) {
  auto it = values_.find(key);
  if (it != values_.end() && it->second == v)
    return false;  // no-op write
  values_[key] = std::move(v);
  dirty_[key] = true;
  removed_.erase(key);
  return true;
}

bool SharedState::erase(const std::string& key) {
  auto it = values_.find(key);
  if (it == values_.end())
    return false;
  values_.erase(it);
  dirty_.erase(key);
  removed_[key] = true;  // tombstone so the removal replicates
  return true;
}

std::vector<std::uint8_t> SharedState::encode_delta() const {
  std::vector<std::uint8_t> out;
  put_varuint(out, dirty_.size());
  for (const auto& [k, _] : dirty_) {
    put_key(out, k);
    const Value* v = get(k);  // dirty keys are always present in values_
    const auto enc = encode(v ? *v : Value::null());
    out.insert(out.end(), enc.begin(), enc.end());
  }
  put_varuint(out, removed_.size());
  for (const auto& [k, _] : removed_)
    put_key(out, k);
  return out;
}

std::vector<std::uint8_t> SharedState::encode_full() const {
  std::vector<std::uint8_t> out;
  put_varuint(out, values_.size());
  for (const auto& [k, v] : values_) {
    put_key(out, k);
    const auto enc = encode(v);
    out.insert(out.end(), enc.begin(), enc.end());
  }
  put_varuint(out, 0);  // no removals in a full snapshot
  return out;
}

bool SharedState::apply_delta(const std::uint8_t* data, std::size_t len,
                              const ChangeFn& on_change) {
  std::size_t pos = 0;

  std::uint64_t n_set = 0;
  if (!get_varuint(data, len, pos, n_set) || n_set > kMaxBagKeys)
    return false;
  for (std::uint64_t i = 0; i < n_set; ++i) {
    std::uint64_t klen = 0;
    if (!get_varuint(data, len, pos, klen))
      return false;
    if (klen > kMaxBagKeyLen || klen > len - pos)
      return false;  // bounded + in-buffer
    std::string key(reinterpret_cast<const char*>(data + pos), static_cast<std::size_t>(klen));
    pos += static_cast<std::size_t>(klen);

    DecodeResult dr = decode(data + pos, len - pos);
    if (!dr.ok)
      return false;
    pos += dr.consumed;

    const Value* cur = get(key);
    const Value old = cur ? *cur : Value::null();
    values_[key] = dr.value;
    if (on_change)
      on_change(key, old, values_[key]);
  }

  std::uint64_t n_del = 0;
  if (!get_varuint(data, len, pos, n_del) || n_del > kMaxBagKeys)
    return false;
  for (std::uint64_t i = 0; i < n_del; ++i) {
    std::uint64_t klen = 0;
    if (!get_varuint(data, len, pos, klen))
      return false;
    if (klen > kMaxBagKeyLen || klen > len - pos)
      return false;
    std::string key(reinterpret_cast<const char*>(data + pos), static_cast<std::size_t>(klen));
    pos += static_cast<std::size_t>(klen);

    auto it = values_.find(key);
    if (it == values_.end())
      continue;  // already absent
    const Value old = it->second;
    values_.erase(it);
    if (on_change)
      on_change(key, old, Value::null());
  }

  return pos == len;  // reject trailing garbage (clean framing)
}

SharedState& SharedStateStore::bag(const std::string& name, BagAuthority auth) {
  auto it = bags_.find(name);
  if (it == bags_.end())
    it = bags_.emplace(name, SharedState(auth)).first;
  return it->second;
}

const SharedState* SharedStateStore::find(const std::string& name) const {
  auto it = bags_.find(name);
  return it == bags_.end() ? nullptr : &it->second;
}

bool SharedStateStore::author_write(const std::string& name, const std::string& key, Value v,
                                    bool is_server) {
  SharedState& b = bag(name);  // new bags default to Server authority
  if (!is_server && b.authority() == BagAuthority::Server)
    return false;  // forged client write
  b.set(key, std::move(v));
  return true;
}

}  // namespace kcd2s::sdk
