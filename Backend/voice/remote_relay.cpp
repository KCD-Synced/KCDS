#include "kcd2s/voice/remote_relay.hpp"

#include <chrono>
#include <utility>

namespace kcd2s::voice {
namespace {

constexpr int kConnectTimeoutMs = 3000;
constexpr int kOpeningTimeoutMs = 5000;
constexpr int kRetryMs = 1000;
constexpr int kBadSecretRetryMs = 10000;
constexpr int kReadSliceMs = 50;
constexpr std::uint64_t kBatchEveryMs = 100;
constexpr std::uint64_t kHeartbeatEveryMs = 1000;
constexpr std::size_t kPositionBytes = 20;

std::uint64_t now_ms() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace

RemoteRelay::RemoteRelay(RemoteRelayOptions options) : options_(std::move(options)) {
}

RemoteRelay::~RemoteRelay() {
  stop();
}

void RemoteRelay::start() {
  if (!thread_.joinable()) {
    thread_ = std::thread([this] { run(); });
  }
}

void RemoteRelay::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

std::optional<RemoteWelcome> RemoteRelay::take_welcome() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(welcome_, std::nullopt);
}

bool RemoteRelay::connected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connected_;
}

void RemoteRelay::log(const std::string& text) const {
  if (options_.log) {
    options_.log(text);
  }
}

void RemoteRelay::post(const ControlMessage& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_) {
    return;
  }
  waiting_bytes_ += sizeof(ControlMessage);
  if (waiting_bytes_ > kMaxWaitingBytes) {
    too_much_waiting_ = true;
    return;
  }
  waiting_.push_back(message);
}

void RemoteRelay::join(core::EntityId player, std::uint32_t session, const Key& client_key) {
  ControlMessage message;
  message.kind = ControlKind::Join;
  message.player = player;
  message.session = session;
  message.key = client_key;
  post(message);
}

void RemoteRelay::mark_synced() {
  ControlMessage message;
  message.kind = ControlKind::Synced;
  post(message);
}

void RemoteRelay::remove_player(core::EntityId player) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    positions_.erase(player);
  }
  ControlMessage message;
  message.kind = ControlKind::Leave;
  message.player = player;
  post(message);
}

void RemoteRelay::set_position(core::EntityId player, const core::Vec3& position,
                               std::uint32_t world) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connected_) {
    positions_[player] = ControlPosition{player, position, world};
  }
}

void RemoteRelay::set_enabled(bool enabled) {
  ControlMessage message;
  message.kind = ControlKind::Enabled;
  message.flag = enabled;
  post(message);
}

void RemoteRelay::set_tier_range(Tier tier, float meters) {
  ControlMessage message;
  message.kind = ControlKind::Range;
  message.tier = static_cast<std::uint8_t>(tier);
  message.meters = meters;
  post(message);
}

void RemoteRelay::set_tier(core::EntityId player, Tier tier) {
  ControlMessage message;
  message.kind = ControlKind::Tier;
  message.player = player;
  message.tier = static_cast<std::uint8_t>(tier);
  post(message);
}

void RemoteRelay::set_muted(core::EntityId player, bool muted) {
  ControlMessage message;
  message.kind = ControlKind::Muted;
  message.player = player;
  message.flag = muted;
  post(message);
}

void RemoteRelay::set_deaf(core::EntityId player, bool deaf) {
  ControlMessage message;
  message.kind = ControlKind::Deaf;
  message.player = player;
  message.flag = deaf;
  post(message);
}

void RemoteRelay::set_listener_mute(core::EntityId listener, core::EntityId talker, bool muted) {
  ControlMessage message;
  message.kind = ControlKind::ListenerMute;
  message.player = listener;
  message.other = talker;
  message.flag = muted;
  post(message);
}

std::vector<TalkChange> RemoteRelay::take_talk_changes() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(talk_changes_, {});
}

void RemoteRelay::wait_before_retry(int milliseconds) {
  std::unique_lock<std::mutex> lock(mutex_);
  wake_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return stopping_; });
}

void RemoteRelay::end_all_talking() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const core::EntityId player : talking_) {
    talk_changes_.push_back(TalkChange{player, false});
  }
  talking_.clear();
}

std::vector<std::uint8_t> RemoteRelay::take_outgoing(ControlSealer& sealer) {
  std::vector<ControlMessage> messages;
  std::map<core::EntityId, ControlPosition> positions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    messages = std::exchange(waiting_, {});
    positions = std::exchange(positions_, {});
    waiting_bytes_ = 0;
  }
  std::vector<std::uint8_t> out;
  const auto add = [&](const ControlMessage& message) {
    const std::vector<std::uint8_t> frame = sealer.seal(encode_control(message));
    out.insert(out.end(), frame.begin(), frame.end());
  };
  for (const ControlMessage& message : messages) {
    add(message);
  }
  // In batches a frame can hold.
  const std::size_t batch_size = (kMaxControlBytes - 16) / kPositionBytes;
  ControlMessage batch;
  batch.kind = ControlKind::Positions;
  for (const auto& [player, entry] : positions) {
    (void)player;
    batch.positions.push_back(entry);
    if (batch.positions.size() == batch_size) {
      add(batch);
      batch.positions.clear();
    }
  }
  if (!batch.positions.empty()) {
    add(batch);
  }
  return out;
}

void RemoteRelay::run() {
  std::string host;
  std::uint16_t port = 0;
  if (!net::split_host_port(options_.url, host, port)) {
    log("voice: the address of the voice server is not host:port");
    return;
  }
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
    }
    const std::optional<std::uint32_t> ip = net::resolve_ipv4(host);
    int retry = kRetryMs;
    if (!ip) {
      log("voice: cannot find the voice server " + host);
    } else if (!run_link(*ip, port)) {
      retry = kBadSecretRetryMs;
    }
    end_all_talking();
    wait_before_retry(retry);
  }
}

bool RemoteRelay::run_link(std::uint32_t ip, std::uint16_t port) {
  const net::Handle socket = net::tcp_connect(ip, port, kConnectTimeoutMs);
  if (socket == net::kNoSocket) {
    return true;
  }
  const auto give_up = [&](bool result) {
    net::close_socket(socket);
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    waiting_.clear();
    waiting_bytes_ = 0;
    too_much_waiting_ = false;
    positions_.clear();
    return result;
  };

  const ControlRandom game_random = make_control_random();
  const std::vector<std::uint8_t> opening = encode_control_opening(options_.server_id, game_random);
  if (!net::tcp_write(socket, opening.data(), opening.size())) {
    return give_up(true);
  }
  ControlRandom voice_random{};
  std::size_t have = 0;
  const std::uint64_t opened_at = now_ms();
  while (have < voice_random.size()) {
    const int got =
        net::tcp_read(socket, voice_random.data() + have, voice_random.size() - have, kReadSliceMs);
    if (got < 0 || now_ms() - opened_at > std::uint64_t(kOpeningTimeoutMs)) {
      log("voice: the voice server refused this server; check the id and the secret");
      return give_up(false);
    }
    have += static_cast<std::size_t>(got);
  }

  ControlSealer sealer(
      control_key(options_.secret, true, options_.server_id, game_random, voice_random));
  ControlOpener opener(
      control_key(options_.secret, false, options_.server_id, game_random, voice_random));
  ControlMessage hello;
  hello.kind = ControlKind::Hello;
  hello.server_id = options_.server_id;
  const std::vector<std::uint8_t> hello_frame = sealer.seal(encode_control(hello));
  if (!net::tcp_write(socket, hello_frame.data(), hello_frame.size())) {
    return give_up(true);
  }

  std::uint8_t buffer[4096];
  std::uint64_t last_heard = now_ms();
  std::uint64_t last_sent = last_heard;
  std::uint64_t last_batch = last_heard;
  bool welcomed = false;
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        break;
      }
      if (too_much_waiting_) {
        log("voice: the link to the voice server is too slow; connecting again");
        break;
      }
    }
    const int got = net::tcp_read(socket, buffer, sizeof(buffer), kReadSliceMs);
    if (got < 0) {
      if (!welcomed) {
        log("voice: the voice server refused this server; check the id and the secret");
        return give_up(false);
      }
      log("voice: the link to the voice server was lost");
      break;
    }
    const std::uint64_t now = now_ms();
    if (got > 0) {
      last_heard = now;
      if (!opener.feed(buffer, static_cast<std::size_t>(got))) {
        log("voice: the voice server sent what the secret does not open");
        return give_up(false);
      }
      while (const auto plain = opener.next()) {
        const std::optional<ControlMessage> message = decode_control(*plain);
        if (!message) {
          continue;
        }
        if (message->kind == ControlKind::Welcome && !welcomed) {
          RemoteWelcome welcome;
          welcome.public_key = message->key;
          welcome.session_base = message->session;
          welcome.endpoint.ip = ip;
          welcome.endpoint.port = net::host_to_network_port(message->port);
          std::string public_host;
          std::uint16_t public_port = 0;
          if (net::split_host_port(options_.public_address, public_host, public_port)) {
            const auto public_ip = net::resolve_ipv4(public_host);
            if (public_ip) {
              welcome.endpoint.ip = *public_ip;
              welcome.endpoint.port = net::host_to_network_port(public_port);
            } else {
              log("voice: cannot find the voice server's public address " + public_host);
            }
          }
          std::lock_guard<std::mutex> lock(mutex_);
          welcome_ = welcome;
          connected_ = true;
          welcomed = true;
          log("voice: linked to the voice server");
        } else if (message->kind == ControlKind::Talking) {
          std::lock_guard<std::mutex> lock(mutex_);
          talk_changes_.push_back(TalkChange{message->player, message->flag});
          if (message->flag) {
            talking_.insert(message->player);
          } else {
            talking_.erase(message->player);
          }
        }
      }
    }
    if (now - last_heard > kSilenceLimitMs) {
      log("voice: the voice server went silent");
      break;
    }
    if (!welcomed) {
      continue;
    }
    if (now - last_batch >= kBatchEveryMs) {
      last_batch = now;
      std::vector<std::uint8_t> out = take_outgoing(sealer);
      if (out.empty() && now - last_sent >= kHeartbeatEveryMs) {
        ControlMessage beat;
        beat.kind = ControlKind::Heartbeat;
        out = sealer.seal(encode_control(beat));
      }
      if (!out.empty()) {
        last_sent = now;
        if (!net::tcp_write(socket, out.data(), out.size())) {
          log("voice: the link to the voice server was lost");
          break;
        }
      }
    }
  }
  return give_up(true);
}

}  // namespace kcd2s::voice
