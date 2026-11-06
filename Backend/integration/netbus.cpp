#include "kcd2s/integration/netbus.hpp"

#include "kcd2s/sdk/marshal.hpp"

namespace kcd2s::integration {

using sdk::Tag;
using sdk::Value;

void NetBus::send(const Value::Array& frame) {
  const std::vector<std::uint8_t> bytes = sdk::encode(Value(frame));
  conn_.queue(channel_, bytes);
}

void NetBus::on(const std::string& event, Handler cb) {
  handlers_[event].push_back(std::move(cb));
}

void NetBus::emit(const std::string& event, const Value::Array& args) {
  send(Value::Array{Value(static_cast<std::int64_t>(kEvent)), Value(event), Value(args)});
}

void NetBus::register_rpc(const std::string& name, RpcHandler fn) {
  rpcs_[name] = std::move(fn);
}

std::uint32_t NetBus::call(const std::string& name, const Value::Array& args,
                           ResultCallback on_result) {
  const std::uint32_t corr = next_corr_++;
  pending_[corr] = std::move(on_result);
  send(Value::Array{Value(static_cast<std::int64_t>(kRpcRequest)),
                    Value(static_cast<std::int64_t>(corr)), Value(name), Value(args)});
  return corr;
}

void NetBus::feed(const std::vector<std::uint8_t>& payload) {
  auto res = sdk::decode(payload);
  if (!res.ok || res.value.tag() != Tag::Array)
    return;
  const Value::Array& f = res.value.as_array();
  if (f.empty() || f[0].tag() != Tag::Int)
    return;

  switch (f[0].as_int()) {
    case kEvent: {
      if (f.size() != 3 || f[1].tag() != Tag::Str || f[2].tag() != Tag::Array)
        return;
      auto it = handlers_.find(f[1].as_str());
      if (it == handlers_.end())
        return;
      // Snapshot handlers so a handler mutating the map mid-dispatch is safe.
      auto hs = it->second;
      for (auto& h : hs)
        h(f[2].as_array());
      break;
    }
    case kRpcRequest: {
      if (f.size() != 4 || f[1].tag() != Tag::Int || f[2].tag() != Tag::Str ||
          f[3].tag() != Tag::Array)
        return;
      const std::int64_t corr = f[1].as_int();
      auto it = rpcs_.find(f[2].as_str());
      Value result = it != rpcs_.end() ? it->second(f[3].as_array()) : Value::null();
      send(Value::Array{Value(static_cast<std::int64_t>(kRpcResponse)), Value(corr), result});
      break;
    }
    case kRpcResponse: {
      if (f.size() != 3 || f[1].tag() != Tag::Int)
        return;
      const std::uint32_t corr = static_cast<std::uint32_t>(f[1].as_int());
      auto it = pending_.find(corr);
      if (it == pending_.end())
        return;
      auto cb = std::move(it->second);
      pending_.erase(it);
      cb(f[2]);
      break;
    }
    default:
      return;
  }
}

}  // namespace kcd2s::integration
