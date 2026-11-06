#include "kcd2s/voice/voice_daemon.hpp"

#include <chrono>
#include <utility>

namespace kcd2s::voice {
namespace {

constexpr int kSliceMs = 50;
constexpr std::uint64_t kOpeningLimitMs = 5000;
constexpr std::uint64_t kHeartbeatEveryMs = 1000;
constexpr std::uint64_t kTalkEveryMs = 50;

std::uint64_t now_ms() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace

VoiceDaemon::VoiceDaemon(VoiceDaemonOptions options) : options_(std::move(options)) {
}

VoiceDaemon::~VoiceDaemon() {
  stop();
}

void VoiceDaemon::log(const std::string& text) const {
  if (options_.log) {
    options_.log(text);
  }
}

bool VoiceDaemon::start() {
  voice_socket_ = net::udp_open(options_.bind, options_.voice_port, voice_port_);
  if (voice_socket_ == net::kNoSocket) {
    log("cannot open the voice port " + std::to_string(options_.voice_port));
    return false;
  }
  control_socket_ = net::tcp_listen(options_.bind, options_.control_port, control_port_);
  if (control_socket_ == net::kNoSocket) {
    log("cannot open the control port " + std::to_string(options_.control_port));
    net::close_socket(voice_socket_);
    voice_socket_ = net::kNoSocket;
    return false;
  }
  tenants_ = std::make_unique<VoiceTenants>(
      RelayConfig{},
      [this](const Address& to, const std::vector<std::uint8_t>& data) {
        net::udp_send(voice_socket_, to, data.data(), data.size());
        datagrams_out_.fetch_add(1, std::memory_order_relaxed);
        bytes_out_.fetch_add(data.size() + 28, std::memory_order_relaxed);
      },
      [] { return now_ms(); }, voice_port_);
  voice_thread_ = std::thread([this] { read_voice(); });
  accept_thread_ = std::thread([this] { accept_links(); });
  tick_thread_ = std::thread([this] { tick_tenants(); });
  log("voice on UDP " + std::to_string(voice_port_) + ", control on TCP " +
      std::to_string(control_port_) + ", " + std::to_string(options_.tenants.size()) +
      " game server(s) allowed");
  return true;
}

void VoiceDaemon::stop() {
  stopping_ = true;
  if (voice_thread_.joinable()) {
    voice_thread_.join();
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  if (tick_thread_.joinable()) {
    tick_thread_.join();
  }
  reap_links(true);
  tenants_.reset();
  net::close_socket(voice_socket_);
  voice_socket_ = net::kNoSocket;
  net::close_socket(control_socket_);
  control_socket_ = net::kNoSocket;
}

std::size_t VoiceDaemon::tenant_count() const {
  return tenants_ ? tenants_->tenants() : 0;
}

std::size_t VoiceDaemon::player_count(const std::string& server_id) const {
  return tenants_ ? tenants_->players(server_id) : 0;
}

bool VoiceDaemon::tenant_enabled(const std::string& server_id) const {
  return tenants_ && tenants_->enabled(server_id);
}

void VoiceDaemon::read_voice() {
  std::uint8_t buffer[kMaxVoiceDatagram + 1];
  while (!stopping_) {
    Address from;
    const int got = net::udp_recv(voice_socket_, buffer, sizeof(buffer), from, kSliceMs);
    if (got > 0 && buffer[0] == kVoiceUpTag) {
      tenants_->offer(from, buffer, static_cast<std::size_t>(got));
    }
  }
}

void VoiceDaemon::tick_tenants() {
  while (!stopping_) {
    tenants_->tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void VoiceDaemon::reap_links(bool all) {
  for (auto it = links_.begin(); it != links_.end();) {
    Link& link = **it;
    if (all || link.finished) {
      if (link.thread.joinable()) {
        link.thread.join();
      }
      it = links_.erase(it);
    } else {
      ++it;
    }
  }
}

void VoiceDaemon::accept_links() {
  while (!stopping_) {
    const net::Handle socket = net::tcp_accept(control_socket_, kSliceMs);
    reap_links(false);
    if (socket == net::kNoSocket) {
      continue;
    }
    if (links_.size() >= kMaxLinks) {
      net::close_socket(socket);
      continue;
    }
    auto link = std::make_unique<Link>();
    Link* raw = link.get();
    link->thread = std::thread([this, socket, raw] {
      run_link(socket);
      net::close_socket(socket);
      raw->finished = true;
    });
    links_.push_back(std::move(link));
  }
}

std::uint64_t VoiceDaemon::claim(const std::string& server_id) {
  std::lock_guard<std::mutex> lock(generations_mutex_);
  const std::uint64_t generation = ++next_generation_;
  generations_[server_id] = generation;
  return generation;
}

bool VoiceDaemon::is_current(const std::string& server_id, std::uint64_t generation) const {
  std::lock_guard<std::mutex> lock(generations_mutex_);
  const auto found = generations_.find(server_id);
  return found != generations_.end() && found->second == generation;
}

void VoiceDaemon::run_link(net::Handle socket) {
  std::uint8_t buffer[4096];
  const std::uint64_t opened_at = now_ms();

  // The opening: who it says it is. Nothing is believed until a frame opens with that one's secret.
  std::vector<std::uint8_t> received;
  std::string server_id;
  ControlRandom game_random{};
  std::size_t used = 0;
  while (true) {
    const int state = decode_control_opening(received, server_id, game_random, used);
    if (state < 0 || stopping_ || now_ms() - opened_at > kOpeningLimitMs) {
      return;
    }
    if (state > 0) {
      break;
    }
    const int got = net::tcp_read(socket, buffer, sizeof(buffer), kSliceMs);
    if (got < 0) {
      return;
    }
    received.insert(received.end(), buffer, buffer + got);
  }
  const auto allowed = options_.tenants.find(server_id);
  if (allowed == options_.tenants.end()) {
    log("refused a link for an unknown game server");
    return;
  }

  const ControlRandom voice_random = make_control_random();
  if (!net::tcp_write(socket, voice_random.data(), voice_random.size())) {
    return;
  }
  ControlOpener opener(control_key(allowed->second, true, server_id, game_random, voice_random));
  ControlSealer sealer(control_key(allowed->second, false, server_id, game_random, voice_random));
  if (received.size() > used && !opener.feed(received.data() + used, received.size() - used)) {
    log("refused a link: wrong secret");
    return;
  }

  // Until the Hello opened, nobody has proven the secret: no tenant is touched.
  bool hello = false;
  std::uint64_t generation = 0;
  std::uint64_t last_heard = now_ms();
  std::uint64_t last_sent = last_heard;
  std::uint64_t last_talk = last_heard;

  const auto send_message = [&](const ControlMessage& message) {
    const std::vector<std::uint8_t> frame = sealer.seal(encode_control(message));
    last_sent = now_ms();
    return net::tcp_write(socket, frame.data(), frame.size());
  };
  const auto read_messages = [&]() {
    while (const auto plain = opener.next()) {
      const std::optional<ControlMessage> message = decode_control(*plain);
      if (!message) {
        continue;
      }
      if (message->kind == ControlKind::Hello) {
        if (hello || message->server_id != server_id) {
          return false;
        }
        hello = true;
        generation = claim(server_id);
        if (!send_message(tenants_->link_opened(server_id))) {
          return false;
        }
        log("game server linked");
      } else if (hello && message->kind != ControlKind::Heartbeat) {
        tenants_->on_control(server_id, *message);
      }
    }
    return true;
  };

  bool alive = read_messages();
  while (alive && !stopping_) {
    if (!hello && now_ms() - opened_at > kOpeningLimitMs) {
      break;
    }
    if (hello && !is_current(server_id, generation)) {
      // A newer link took over; it alone speaks for the tenant.
      return;
    }
    const int got = net::tcp_read(socket, buffer, sizeof(buffer), kSliceMs);
    if (got < 0) {
      break;
    }
    const std::uint64_t now = now_ms();
    if (got > 0) {
      last_heard = now;
      if (!opener.feed(buffer, static_cast<std::size_t>(got))) {
        log(hello ? "dropped a link: a bad frame" : "refused a link: wrong secret");
        break;
      }
      alive = read_messages();
    }
    if (!hello) {
      continue;
    }
    if (now - last_heard > kSilenceLimitMs) {
      log("a game server went silent");
      break;
    }
    if (now - last_talk >= kTalkEveryMs) {
      last_talk = now;
      for (const ControlMessage& change : tenants_->take_talking(server_id)) {
        alive = alive && send_message(change);
      }
    }
    if (now - last_sent >= kHeartbeatEveryMs) {
      ControlMessage beat;
      beat.kind = ControlKind::Heartbeat;
      alive = alive && send_message(beat);
    }
  }
  if (hello && is_current(server_id, generation)) {
    log("game server link lost");
    tenants_->link_lost(server_id);
  }
}

}  // namespace kcd2s::voice
