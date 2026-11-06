// Networked events + RPC over KSTP channel 2 (events, reliable-ordered) — Args are
// marshaled with the canonical cross-language Value (Backend/sdk/marshal), so emits and RPC are
// language- independent end to end. This is the transport-facing half; the local ResourceHost bus
// dispatches to script resources. NetBus owns neither the receive loop nor flushing: the owner
// drains the Connection and routes channel-2 payloads to feed(); emit/call queue onto the
// Connection.
#ifndef KCD2S_INTEGRATION_NETBUS_HPP
#define KCD2S_INTEGRATION_NETBUS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "kcd2s/sdk/value.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/wire/packet.hpp"

namespace kcd2s::integration {

class NetBus {
 public:
  using Handler = std::function<void(const sdk::Value::Array& args)>;
  using RpcHandler = std::function<sdk::Value(const sdk::Value::Array& args)>;
  using ResultCallback = std::function<void(const sdk::Value& result)>;

  explicit NetBus(transport::Connection& conn,
                  std::uint8_t channel = static_cast<std::uint8_t>(wire::Channel::Events))
      : conn_(conn), channel_(channel) {
  }

  // Subscribe to a networked event. Multiple handlers per name run in registration order.
  void on(const std::string& event, Handler cb);
  // Send an event to the peer (queued on the Connection; delivered on the next flush).
  void emit(const std::string& event, const sdk::Value::Array& args = {});

  // Register an RPC the peer can call. One handler per name (last registration wins).
  void register_rpc(const std::string& name, RpcHandler fn);
  // Call a peer RPC; `on_result` fires when the response arrives. Returns the correlation id.
  std::uint32_t call(const std::string& name, const sdk::Value::Array& args,
                     ResultCallback on_result);

  // Dispatch one inbound channel-2 payload (the owner routes Connection messages here). Ignores
  // malformed payloads.
  void feed(const std::vector<std::uint8_t>& payload);

  std::size_t pending_calls() const {
    return pending_.size();
  }

 private:
  enum Kind : std::int64_t { kEvent = 0, kRpcRequest = 1, kRpcResponse = 2 };
  void send(const sdk::Value::Array& frame);

  transport::Connection& conn_;
  std::uint8_t channel_;
  std::uint32_t next_corr_ = 1;
  std::unordered_map<std::string, std::vector<Handler>> handlers_;
  std::unordered_map<std::string, RpcHandler> rpcs_;
  std::unordered_map<std::uint32_t, ResultCallback> pending_;
};

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_NETBUS_HPP
