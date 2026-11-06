#include "kcd2s/integration/shared_state_sync.hpp"

#include <algorithm>

#include "kcd2s/sdk/marshal.hpp"

namespace kcd2s::integration {

using sdk::SharedState;
using sdk::Tag;
using sdk::Value;

namespace {

// Pack a SharedState payload (delta or full snapshot) as a NetBus event frame's args.
Value::Array delta_args(const std::string& bag, bool full, const std::vector<std::uint8_t>& bytes) {
  return Value::Array{Value(bag), Value(full), Value::bytes(bytes)};
}

}  // namespace

void ServerSharedStates::add_client(NetBus& bus) {
  buses_.push_back(&bus);
  // A late joiner needs the current world: full snapshot of every bag.
  send_snapshot_to(bus);
  // Honor this client's write proposals through the store's authority guard.
  bus.on(kSbWriteEvent, [this](const Value::Array& a) {
    if (a.size() != 3 || a[0].tag() != Tag::Str || a[1].tag() != Tag::Str ||
        a[2].tag() != Tag::Bytes)
      return;
    auto dr = sdk::decode(a[2].as_bytes());
    if (!dr.ok)
      return;
    // is_server=false: a write to a Server-authority bag is rejected here.
    if (store_.author_write(a[0].as_str(), a[1].as_str(), dr.value, /*is_server=*/false))
      broadcast_dirty();  // accepted (Client-authority bag) -> fan out immediately
  });
}

void ServerSharedStates::set(const std::string& bag, const std::string& key, Value v,
                             sdk::BagAuthority auth) {
  store_.bag(bag, auth).set(key, std::move(v));
}

void ServerSharedStates::erase(const std::string& bag, const std::string& key) {
  auto& b = store_.bag(bag);
  b.erase(key);  // records a dirty tombstone; broadcast_dirty() ships the removal
}

void ServerSharedStates::remove_client(NetBus& bus) {
  buses_.erase(std::remove(buses_.begin(), buses_.end(), &bus), buses_.end());
}

void ServerSharedStates::send_snapshot_to(NetBus& bus) {
  for (auto& [name, b] : store_.bags())
    bus.emit(kSbDeltaEvent, delta_args(name, /*full=*/true, b.encode_full()));
}

void ServerSharedStates::broadcast_dirty() {
  for (auto& [name, b] : store_.bags()) {
    if (!b.dirty())
      continue;
    const auto payload = b.encode_delta();
    for (NetBus* bus : buses_)
      bus->emit(kSbDeltaEvent, delta_args(name, /*full=*/false, payload));
    b.clear_dirty();
  }
}

ClientSharedStates::ClientSharedStates(sdk::SharedStateStore& store, NetBus& bus,
                                       ChangeFn on_change)
    : store_(store), bus_(bus), on_change_(std::move(on_change)) {
  bus_.on(kSbDeltaEvent, [this](const Value::Array& a) {
    if (a.size() != 3 || a[0].tag() != Tag::Str || a[1].tag() != Tag::Bool ||
        a[2].tag() != Tag::Bytes)
      return;
    const std::string& name = a[0].as_str();
    SharedState& b = store_.bag(name);  // server-sourced: default Server authority locally
    SharedState::ChangeFn cb;
    if (on_change_) {
      cb = [this, &name](const std::string& key, const Value& o, const Value& n) {
        on_change_(name, key, o, n);
      };
    }
    b.apply_delta(a[2].as_bytes(), cb);
  });
}

void ClientSharedStates::propose(const std::string& bag, const std::string& key, Value v) {
  bus_.emit(kSbWriteEvent, Value::Array{Value(bag), Value(key), Value::bytes(sdk::encode(v))});
}

}  // namespace kcd2s::integration
