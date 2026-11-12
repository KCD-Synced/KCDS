#pragma once
#ifdef _WIN32
// winsock2.h (udp_win.hpp) must come before Windows.h (script_process.hpp).
#include "udp_win.hpp"
#include "script_process.hpp"
#else
#include "script_process_posix.hpp"
#include "udp_posix.hpp"
#endif
#include "movement.hpp"
#include "voice_host.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/bot_admission.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/transport/fragment.hpp"
#include "kcd2s/integration/departure.hpp"
#include "kcd2s/integration/ground_items.hpp"
#include "kcd2s/integration/item_types.hpp"
#include "kcd2s/integration/physical_prop.hpp"
#include "kcd2s/integration/lead_rope.hpp"
#include "kcd2s/integration/pose_animation.hpp"
#include "kcd2s/integration/stash.hpp"
#include "kcd2s/integration/carry.hpp"
#include "kcd2s/integration/hands.hpp"
#include "kcd2s/integration/horse_buffs.hpp"
#include "kcd2s/integration/nameplates.hpp"
#include "kcd2s/integration/player_slots.hpp"
#include "kcd2s/transport/disconnect.hpp"
#include "kcd2s/transport/handshake.hpp"
#include "kcd2s/wire/packet.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string_view>
#include <random>
#include <set>
#include <cmath>
#include <thread>
#include "metrics/counters.hpp"
#include "metrics/duration_histogram.hpp"
#include "metrics/metrics_settings.hpp"
#include "config/host_settings.hpp"
#include "maps/server_maps.hpp"
#include "resources/download_cache.hpp"
#include "resources/http_files.hpp"
#include "resources/report_cache.hpp"
#include "resources/view_reports.hpp"

namespace kcd2s::local {
inline double clock_seconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::uint64_t nonce() {
  std::random_device r;
  return (std::uint64_t(r()) << 32) ^ r();
}
// Read-only view of Host queues and per-peer traffic, built on demand for opt-in metrics.
struct HostPeerTraffic {
  core::PeerId peer{};
  std::uint64_t packets_sent = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t packets_received = 0;
  std::uint64_t bytes_received = 0;
};
struct HostQueueView {
  std::size_t peers = 0;
  std::size_t inbox_total = 0;
  std::size_t inbox_max = 0;
  std::size_t outstanding_reliable_total = 0;
  // Reliable message copies sent and retransmitted since start (departed members included).
  std::uint64_t reliable_sent_total = 0;
  std::uint64_t reliable_retransmits_total = 0;
  std::uint64_t snapshot_bytes_total = 0;
  // KSTP datagrams above the safe UDP payload that were split, and the fragments sent for them
  // (cumulative, departed members included).
  std::uint64_t fragmented_datagrams_total = 0;
  std::uint64_t fragments_total = 0;
  std::vector<HostPeerTraffic> peer_traffic;
};
// Per-phase durations of Host::tick, recorded only after enable_phase_timing().
struct HostPhaseTimes {
  metrics::DurationHistogram ingress;         // socket drain, handshakes, inbox routing
  metrics::DurationHistogram timeouts;        // idle-member removal
  metrics::DurationHistogram server;          // Server::tick: receive, snapshot build, flush/send
  metrics::DurationHistogram server_receive;  // part of `server`: what every peer sent (serial)
  metrics::DurationHistogram snapshot_build;  // part of `server`: building every peer's snapshot
  metrics::DurationHistogram snapshot_send;   // part of `server`: queueing and flushing (serial)
  metrics::DurationHistogram resources;       // resource host bridge
  void reset() {
    ingress.reset();
    timeouts.reset();
    server.reset();
    server_receive.reset();
    snapshot_build.reset();
    snapshot_send.reset();
    resources.reset();
  }
};
// Resource host bridge: IPC traffic and what the view reports track.
struct HostResourceView {
  bool running = false;
  ScriptBridgeCounters bridge;
  std::size_t tracked_entities = 0;
  std::size_t tracked_views = 0;
};
// Number of members removed per departure reason since the host started.
struct HostDepartureCounts {
  std::uint64_t timeout = 0;
  std::uint64_t disconnect = 0;
  std::uint64_t kicked = 0;
};
class Host {
 public:
  // Over-capacity handshakes get CONNECT_REFUSED (server full) with this retry hint. Refusals are
  // limited globally to kRefusalsPerSecond with a burst of kRefusalBurst; beyond that they are
  // left unanswered. Refusals are smaller than the handshake they answer (no amplification).
  static constexpr std::uint16_t kRefusalRetryAfterMs = 5000;
  static constexpr double kRefusalsPerSecond = 128.0;
  static constexpr double kRefusalBurst = 128.0;
  // A client of another build hears the server's version once per address in this time; the
  // addresses remembered for it are capped.
  static constexpr double kOtherBuildAnswerSeconds = 2.0;
  static constexpr std::size_t kOtherBuildAddresses = 4096;
  // Seconds without a valid packet before a member is removed (fallback for a lost or missing
  // disconnect notice).
  static constexpr double kDefaultInactivityTimeout = 10.0;
  // A connection that has not entered the world yet may be silent much longer: after the
  // handshake the game sets itself up (hooks, HUD, map) for 10-20 s before its network runs.
  static constexpr double kDefaultJoiningInactivityTimeout = 60.0;

  // Partial reassemblies kept per member and the largest datagram a member may send in fragments.
  // Client datagrams carry input, acknowledgements and bounded script chunks, far below this.
  static constexpr std::size_t kMemberMaxPartials = 8;
  static constexpr std::uint32_t kMemberMaxMessageBytes = 64 * 1024;
  static constexpr std::uint32_t kReassemblyTimeoutTicks = 60;
  // The host drains its socket once per tick. With the default buffer (about 64 KiB on Windows)
  // the OS silently dropped about half of the client input at 100 peers on loopback.
  static constexpr int kSocketBufferBytes = 8 * 1024 * 1024;
  // Datagrams the receive thread keeps for the next ticks (about a second of 1,000 players).
  static constexpr std::size_t kReaderQueueDatagrams = 65536;

  Host(const char* ip, unsigned short port, integration::WorldConfig config = {},
       config::HostSettings settings = {})
      : schema_(player_schema()),
        socket_(address(ip, port)),
        gate_(nonce()),
        refusal_budget_(kRefusalsPerSecond, kRefusalBurst),
        settings_(settings),
        server_(schema_,
                replication::InterestConfig{settings.interest_cell_size(), replication::UpAxis::Z}),
        config_(std::move(config)) {
    if (!integration::valid_world_config(config_))
      throw std::runtime_error("Unsupported world configuration");
    maps_ = {config_.map};
    if (!socket_.set_buffer_bytes(kSocketBufferBytes, kSocketBufferBytes))
      std::cerr << "UDP socket buffer size request refused" << std::endl;
    // The first tick's ingress and snapshot run at tick 1; see tick() for when it advances.
    server_.world().set_tick(1);
    server_.set_interest_hysteresis(settings_.interest_hysteresis);
    server_.set_character_limit(settings_.max_characters);
    server_.set_snapshot_budget(settings_.snapshot_budget());
    server_.set_distance_rates(settings_.distance_rates);
    server_.set_movement_check(settings_.movement_check);
    server_.set_melee_reach_check(settings_.melee_reach_check);
    if (settings_.parallel_snapshots())
      server_.enable_parallel(settings_.snapshot_workers);
    // Before the resources' ids, so the first player of a session is 1.
    reserve_player_ids();
  }
  const config::HostSettings& settings() const {
    return settings_;
  }
  // The server program reads its socket on a thread of its own (UdpSocket::start_reader), so a
  // tick does not spend one recvfrom call per datagram. Hosts in tests read inline: what was
  // sent before a tick is there in that tick.
  void start_receive_thread() {
    voice_.set_muffle_sender(
        [this](core::EntityId player, core::EntityId talker, std::uint8_t amount) {
          return server_.send_voice_muffle(player, talker, amount);
        });
    if (settings_.voice_external) {
      voice::RemoteRelayOptions voice_link;
      voice_link.url = settings_.voice_url;
      voice_link.server_id = settings_.voice_server_id;
      voice_link.secret = settings_.voice_secret;
      voice_link.public_address = settings_.voice_public_address;
      voice_link.log = [](const std::string& text) { std::cout << text << std::endl; };
      voice_.use_external(std::move(voice_link));
    }
    voice_.start_thread();
    socket_.set_datagram_tap([this](const sockaddr_in& from, const Datagram& d) {
      if (!voice::is_voice_datagram(d.data(), d.size())) {
        return false;
      }
      voice_.offer(from.sin_addr.s_addr, from.sin_port, d.data(), d.size());
      return true;
    });
    socket_.start_reader(kReaderQueueDatagrams);
  }
  std::uint64_t receive_thread_drops() const {
    return socket_.reader_drops();
  }
  const integration::SnapshotBudgetCounters& snapshot_budget_counters() const {
    return server_.budget_counters();
  }
  integration::CharacterAdmissionStats character_admission_totals() const {
    return server_.character_admission_totals();
  }
  // Network ids the resource host names its own entities (horses) with; refilled on request.
  static constexpr std::uint32_t kScriptEntityIdBlock = 4096;
  // The first id of a new block of kScriptEntityIdBlock ids, or kInvalidEntity when the session's
  // ids are exhausted. They follow the players' first ids.
  core::EntityId reserve_script_ids() {
    return server_.world().reserve_ids(kScriptEntityIdBlock);
  }
  void start_resources(const ProcessText& node, const ProcessText& entry,
                       const ProcessText& folder) {
    std::vector<ProcessText> arguments{entry, folder};
    const auto first = reserve_script_ids();
    if (first != core::kInvalidEntity)
      arguments.push_back(
          process_text(std::to_string(first) + ":" + std::to_string(kScriptEntityIdBlock)));
    // The map before the resources start, so World.level and the level's areas are ready in
    // resourceStart; hostSettings below repeats it.
    if (!config_.map.empty())
      arguments.push_back(process_text("--map=" + config_.map));
    // Every map of a server that runs more than one, in order (the resource host's World.maps).
    std::string all_maps;
    for (const std::string& map : maps_) {
      all_maps += (all_maps.empty() ? "" : ",") + map;
    }
    if (maps_.size() > 1)
      arguments.push_back(process_text("--maps=" + all_maps));
#ifdef KCDS_SERVER_VERSION
    // The KCDS version, before the resources start (Runtime.serverVersion and apiVersion).
    arguments.push_back(process_text(std::string("--version=") + KCDS_SERVER_VERSION));
#endif
    scripts_ = std::make_unique<ScriptProcess>(node, arguments);
    // A new resource host holds no views, players or horses yet.
    view_reports_.reset();
    for (auto& [address, member] : members_) {
      (void)address;
      member.reported_view_revision = 0;
    }
    player_reports_.clear();
    progression_reported_.clear();
    horse_reports_.clear();
    npc_reports_.clear();
    // The SDK entity sender sizes its peer table and budgets from the same connection cap; the
    // licence gate checks an unlisted server's passes against the port players dial; the level's
    // own areas are those of the server's map; the status API tells the version and
    // the protocol.
    Value::Map host_settings{{"op", Value("hostSettings")},
                             {"maxPeers", Value(std::int64_t(settings_.max_peers))},
                             {"port", Value(std::int64_t(port()))},
                             {"map", Value(config_.map)},
                             {"protocol", Value(std::int64_t(wire::kProtocolVersion))}};
    Value::Array map_names;
    for (const std::string& map : maps_) {
      map_names.emplace_back(map);
    }
    host_settings.emplace_back("maps", Value(std::move(map_names)));
#ifdef KCDS_SERVER_VERSION
    host_settings.emplace_back("version", Value(std::string(KCDS_SERVER_VERSION)));
#endif
    scripts_->send(Value(std::move(host_settings)));
  }
  // The id the Master lists this server under (its heartbeat answer): the server a licence pass
  // must name. False when the scripts could not be told yet.
  void require_licence_verifier(bool required) {
    // The server executable latches this before accepting connections, independently
    // of listing state. Only low-level headless transport fixtures leave it false.
    licence_listed_ = licence_listed_ || required;
  }
  void require_master_listing() {
    licence_listed_ = true;
    licence_listing_pending_ = true;
  }
  // The maps the server runs (maps/server_maps.hpp), its own level first; before
  // start_resources, so the resource host knows them. Without a call the server runs config_.map.
  void set_maps(std::vector<std::string> maps) {
    if (maps.empty() || maps.front() != config_.map) {
      throw std::runtime_error("The server's maps start with its level");
    }
    maps_ = std::move(maps);
  }
  // The server's name for the players' watermarks.
  void set_server_name(std::string name) {
    server_.set_server_name(std::move(name));
  }
  bool master_listing(const std::string& id) {
    if (!id.empty()) {
      licence_listed_ = true;
    }
    if (!scripts_ || !scripts_->alive() || id.empty() || id.size() > 128) {
      return false;
    }
    if (!std::all_of(id.begin(), id.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        })) {
      return false;
    }
    if (!scripts_->send(sdk::Value(
            sdk::Value::Map{{"op", sdk::Value("masterListing")}, {"id", sdk::Value(id)}}))) {
      return false;
    }
    licence_listing_pending_ = false;
    return true;
  }
  // A metrics interval (metrics/server_metrics.hpp) for the resource host, which serves it as
  // Prometheus text at /metrics and as the health in /dynamic.json.
  void publish_metrics(const std::string& json) {
    if (scripts_ && scripts_->alive())
      scripts_->send(sdk::Value(
          sdk::Value::Map{{"op", sdk::Value("serverMetrics")}, {"json", sdk::Value(json)}}));
  }
  // The server's validation counters: movement steps and melee contacts it refused.
  std::uint64_t movement_refusals() const {
    return server_.movement_refusals();
  }
  std::uint64_t melee_refusals() const {
    return server_.melee_refusals();
  }
  void console_command(const std::string& text) {
    if (scripts_ && text.size() <= 1024)
      scripts_->send(
          sdk::Value(sdk::Value::Map{{"op", sdk::Value("console")}, {"text", sdk::Value(text)}}));
  }
  // Opt-in instrumentation. Counters always run (plain increments); per-phase timing is off by
  // default and adds clock reads to tick() only after this call.
  void enable_phase_timing() {
    phase_timing_ = true;
  }
  HostPhaseTimes& phase_times() {
    return phase_times_;
  }
  const metrics::HostCounters& counters() const {
    return counters_;
  }
  const metrics::TrafficCounters& socket_traffic() const {
    return socket_.traffic();
  }
  int socket_receive_buffer_bytes() const {
    return socket_.receive_buffer_bytes();
  }
  int socket_send_buffer_bytes() const {
    return socket_.send_buffer_bytes();
  }
  HostQueueView queue_view() const {
    HostQueueView view;
    view.peers = members_.size();
    view.outstanding_reliable_total = server_.outstanding_reliable_total();
    const auto reliable = server_.reliable_counters();
    view.reliable_sent_total = reliable.sent;
    view.reliable_retransmits_total = reliable.retransmits;
    view.snapshot_bytes_total = server_.snapshot_bytes_sent();
    view.fragmented_datagrams_total = departed_fragmented_datagrams_;
    view.fragments_total = departed_fragments_;
    view.peer_traffic.reserve(members_.size());
    for (const auto& [address, m] : members_) {
      (void)address;
      view.fragmented_datagrams_total += m.frames->fragmented_datagrams();
      view.fragments_total += m.frames->fragments_sent();
      const std::size_t depth = m.link->inbox.size();
      view.inbox_total += depth;
      if (depth > view.inbox_max)
        view.inbox_max = depth;
      HostPeerTraffic traffic;
      traffic.peer = m.peer;
      traffic.packets_sent = m.link->sent_packets();
      traffic.bytes_sent = m.link->sent_bytes();
      traffic.packets_received = m.received.packets_received;
      traffic.bytes_received = m.received.bytes_received;
      view.peer_traffic.push_back(traffic);
    }
    return view;
  }
  HostResourceView resource_view() const {
    HostResourceView view;
    view.running = scripts_ != nullptr;
    if (scripts_) {
      view.bridge = scripts_->counters();
    }
    view.tracked_entities = view_reports_.tracked_entities();
    view.tracked_views = view_reports_.tracked_peers();
    return view;
  }
  // Idle limit of connections in the world, and of joining ones (tests set both).
  void set_inactivity_timeout(double seconds) {
    inactivity_timeout_ = seconds;
    joining_inactivity_timeout_ = seconds;
  }
  unsigned short port() const {
    return socket_.local_port();
  }
  // The resource host asked the server to stop (KCDS-Admin, a scheduled restart; issue):
  // what the players are told, and whether the server is to start again (its exit code
  // tells the start script). Returned once.
  struct StopRequest {
    std::string reason;
    bool restart = false;
  };
  std::optional<StopRequest> take_stop_request() {
    return std::exchange(stop_request_, std::nullopt);
  }
  std::size_t member_count() const {
    return members_.size();
  }
  // Player.playerIndex of a connected avatar, or PlayerSlots::kUnassigned.
  std::uint16_t player_index(core::EntityId avatar) const {
    return player_slots_.slot_of(avatar);
  }
  bool entity_alive(core::EntityId entity) const {
    return server_.world().alive(entity);
  }
  const HostDepartureCounts& departures() const {
    return departures_;
  }
  // Tells every connected player's client that the server is shutting down, and why (SESSION_END).
  // The caller keeps ticking for a moment so the messages go out.
  void end_sessions(const std::string& reason) {
    // The resource host hears that the server stops in order: no crash report.
    if (scripts_) {
      scripts_->send(Value(Value::Map{{"op", Value("serverStopping")}}));
    }
    for (const auto& [address, member] : members_) {
      (void)address;
      server_.send_session_end(member.avatar, {integration::SessionEndKind::Closed, reason});
    }
  }
  void tick(double now) {
    const double tick_started = clock_seconds();
    const double phase_start = phase_timing_ ? tick_started : 0.0;
    sockaddr_in from{};
    Datagram d;
    const unsigned receive_budget = settings_.receive_budget();
    unsigned budget = 0;
    for (; budget < receive_budget && socket_.recv(from, d); ++budget) {
      // Without the reader thread (a test host), voice comes this way.
      if (voice::is_voice_datagram(d.data(), d.size())) {
        voice_.offer(from.sin_addr.s_addr, from.sin_port, d.data(), d.size());
        continue;
      }
      auto k = key(from);
      auto it = members_.find(k);
      // A disconnect notice is never routed as channel traffic. Non-members and notices that do
      // not match the member's own session are ignored without a reply.
      if (transport::is_disconnect_notice_frame(d)) {
        if (it != members_.end() && accepts_disconnect_notice(it->second, d))
          depart(it, integration::DepartureReason::Disconnect);
        continue;
      }
      // A normal packet has a protocol header; only exact handshake frames use the gate.
      bool handshake =
          (d.size() == transport::kConnectRequestSize && d[0] == transport::kConnectRequest) ||
          (d.size() == transport::kChallengeResponseSize && d[0] == transport::kChallengeResponse);
      // The client's identity rides with every response it sends.
      if (it == members_.end() && transport::handshake_kind(d) == transport::kJoinIdentity) {
        remember_identity(k, d, now);
        continue;
      }
      if (handshake) {
        // A client of another build is told this server's version.
        if (it == members_.end() && d[0] == transport::kConnectRequest &&
            answer_other_build(k, from, d, now))
          continue;
        // A full server lets the joiner wait in its queue: the request still gets
        // its challenge, the response waits for its turn.
        if (it == members_.end() && d[0] == transport::kChallengeResponse &&
            waits_in_queue(k, from, d, now))
          continue;
        // playerConnecting: the last handshake frame of a new connection waits until the
        // resources decided. The client keeps retransmitting it; nothing is accepted meanwhile.
        if (it == members_.end() && d[0] == transport::kChallengeResponse &&
            !admission_allows(k, from, d, now))
          continue;
        Datagram reply;
        if (gate_.on_datagram(k, d, reply))
          socket_.send(from, reply);
        if (gate_.admitted(k) && it == members_.end()) {
          const core::EntityId avatar = spawn_member_avatar();
          if (avatar == core::kInvalidEntity) {
            // Entity ids of this session are exhausted (or the world is full). Fail closed: never
            // admit a member without an avatar; the address must handshake again later.
            gate_.forget(k);
            std::cout << "JOIN refused: no entity id available" << std::endl;
            continue;
          }
          Member m;
          m.link = std::make_unique<QueueLink>(socket_, from);
          // KSTP datagrams above the safe UDP payload are split into verbatim fragments.
          m.frames = std::make_unique<transport::FragmentingLink>(
              m.link.get(), settings_.max_datagram_bytes, transport::FragmentFraming::Verbatim);
          m.frames->set_max_partials(kMemberMaxPartials);
          m.frames->set_max_message_bytes(kMemberMaxMessageBytes);
          m.frames->set_reassembly_timeout(kReassemblyTimeoutTicks);
          m.avatar = avatar;
          m.player_index = player_slots_.assign(m.avatar);
          server_.world().write(m.avatar, PlayerIndex, std::int64_t(m.player_index));
          m.peer = server_.add_client(m.frames.get(), m.avatar, settings_.interest_radius);
          m.report_slice = next_report_slice_++;
          m.session = transport::session_credentials_from_handshake(d);
          server_.configure_world(m.peer, config_);
          m.world_revision = config_.revision;
          server_.set_owner(m.avatar, m.peer);
          m.last = now;
          std::cout << "JOIN peer=" << m.peer << " avatar=" << m.avatar
                    << " playerIndex=" << m.player_index << std::endl;
          if (const auto admitted = admitted_requests_.find(k);
              admitted != admitted_requests_.end()) {
            if (scripts_ && scripts_->alive()) {
              scripts_->send(Value(Value::Map{{"op", Value("playerAdmitted")},
                                              {"request", Value(std::int64_t(admitted->second))},
                                              {"id", Value(std::int64_t(m.avatar))}}));
            }
            admitted_requests_.erase(admitted);
          }
          members_.emplace(k, std::move(m));
          counters_.joins += 1;
        }
      } else if (it != members_.end()) {
        wire::Packet packet;
        const bool framed = d.size() > 1 && transport::is_verbatim_frame_tag(d[0]);
        if (framed || wire::read_packet(d.data(), d.size(), packet)) {
          it->second.last = now;
          it->second.received.record_received(d.size());
          if (it->second.link->inbox.size() < 256)
            it->second.link->inbox.push_back(std::move(d));
          else
            counters_.inbox_overflow_drops += 1;
        }
      }
    }
    if (budget == receive_budget)
      counters_.receive_budget_exhausted_ticks += 1;
    for (auto& [address, m] : members_) {
      (void)address;
      m.frames->poll();  // expire incomplete fragment sets
    }
    const double ingress_end = phase_timing_ ? clock_seconds() : 0.0;
    for (auto it = members_.begin(); it != members_.end();) {
      const double limit = server_.peer_world_ready(it->second.peer) ? inactivity_timeout_
                                                                     : joining_inactivity_timeout_;
      const auto kick = kicks_.find(it->second.avatar);
      if (now - it->second.last > limit) {
        it = depart(it, integration::DepartureReason::Timeout);
      } else if (kick != kicks_.end() && now >= kick->second) {
        it = depart(it, integration::DepartureReason::Kicked);
      } else {
        ++it;
      }
    }
    const double timeouts_end = phase_timing_ ? clock_seconds() : 0.0;
    server_.tick(now);
    const double server_end = phase_timing_ ? clock_seconds() : 0.0;
    // Advance the tick after this tick's snapshots were built, so writes by the resources (a
    // teleport, a field set by a script) are stamped with the next snapshot's tick. Stamped with
    // the tick already sent, a change was skipped once the client acknowledged that snapshot, and
    // an owner re-sending the same value never stamped it again (live 0.1.1: /tp stayed invisible
    // to other players until the teleported player moved).
    server_.world().set_tick(server_.world().tick() + 1);
    tick_resources();
    plan_snapshot_builds(clock_seconds() - tick_started);
    if (phase_timing_) {
      const double resources_end = clock_seconds();
      phase_times_.ingress.add((ingress_end - phase_start) * 1000.0);
      phase_times_.timeouts.add((timeouts_end - ingress_end) * 1000.0);
      phase_times_.server.add((server_end - timeouts_end) * 1000.0);
      phase_times_.server_receive.add(server_.last_receive_seconds() * 1000.0);
      phase_times_.snapshot_build.add(server_.last_build_seconds() * 1000.0);
      phase_times_.snapshot_send.add(server_.last_send_seconds() * 1000.0);
      phase_times_.resources.add((resources_end - server_end) * 1000.0);
    }
  }

 private:
  using Value = sdk::Value;
  // Player ids (Player.id): the first player of a session gets 1, the next 2, and so
  // on, out of ids the world reserves for players only. Like every entity id, a player's id is
  // never handed to a later player in the same session (a stale id must not name someone else);
  // a used-up block is followed by a new one, after the ids handed out meanwhile.
  static constexpr std::uint32_t kPlayerEntityIdBlock = 4096;
  void reserve_player_ids() {
    const auto first = server_.world().reserve_ids(kPlayerEntityIdBlock);
    if (first == core::kInvalidEntity)
      return;
    next_player_id_ = first;
    player_ids_left_ = kPlayerEntityIdBlock;
  }
  // A joining member's avatar under the next player id, or kInvalidEntity when the session's ids
  // are exhausted or the world is full.
  core::EntityId spawn_member_avatar() {
    if (player_ids_left_ == 0)
      reserve_player_ids();
    if (player_ids_left_ == 0)
      return core::kInvalidEntity;
    const core::EntityId avatar = spawn_player(server_.world(), next_player_id_);
    if (avatar != core::kInvalidEntity) {
      ++next_player_id_;
      --player_ids_left_;
    }
    return avatar;
  }
  // Explicit, stateless "server full" reply to a valid handshake from a non-member. Invalid or
  // foreign handshakes are still ignored, and the global budget bounds the reply rate.
  // Admission of a new connection through the resources' playerConnecting handlers. Without a
  // resource host no request has to wait.
  struct PendingAdmission {
    std::uint32_t request = 0;
    double started = 0;
    transport::JoinIdentity identity;
    bool decided = false;
    bool allowed = false;
    bool timed_out = false;
    // Refused for the server's password: the client asks the player for it.
    bool password = false;
    // The server's own bans turned it away (RefusalReason::Banned).
    bool banned = false;
    // No verified Discord account on a server that asks for one (RefusalReason::DiscordRequired).
    bool discord = false;
    std::string reason;  // connection.reject(reason): shown to the refused player
    std::string line;    // connection.update(message): shown to the waiting player
  };
  static constexpr double kAdmissionTimeoutSeconds = 30.0;

  // The join queue of a full server: who waits for a place, by address, in the
  // order the resources give (a higher priority first, then who came first). A waiting client
  // keeps sending its CHALLENGE_RESPONSE; every one is answered with a WAIT_STATUS (its place, how
  // long it waited, the resources' line and fields). One that stops sending is forgotten.
  struct QueueEntry {
    std::uint32_t id = 0;
    double since = 0;
    double heard = 0;
    transport::JoinIdentity identity;
    std::int64_t priority = 0;
    std::string line;
    std::vector<transport::WaitField> fields;
    bool rejected = false;
    std::string reason;
  };
  static constexpr double kQueueSilenceSeconds = 15.0;
  static constexpr std::size_t kMaxQueue = 1024;
  static constexpr std::size_t kMaxIdentities = 4096;
  bool licence_listed_ = false;
  bool licence_listing_pending_ = false;
  // Address -> the admission request let in, until its member exists.
  std::unordered_map<std::uint64_t, std::uint32_t> admitted_requests_;
  struct HeardIdentity {
    transport::JoinIdentity identity;
    double at = 0;
  };
  void remember_identity(std::uint64_t source, const Datagram& d, double now) {
    auto identity = transport::decode_join_identity(d);
    if (!identity) {
      return;
    }
    if (identities_.size() >= kMaxIdentities && identities_.count(source) == 0) {
      for (auto stale = identities_.begin(); stale != identities_.end();) {
        if (now - stale->second.at > kQueueSilenceSeconds) {
          stale = identities_.erase(stale);
        } else {
          ++stale;
        }
      }
      if (identities_.size() >= kMaxIdentities) {
        return;
      }
    }
    identities_[source] = HeardIdentity{std::move(*identity), now};
  }
  transport::JoinIdentity identity_of(std::uint64_t source) const {
    auto found = identities_.find(source);
    return found == identities_.end() ? transport::JoinIdentity{} : found->second.identity;
  }
  void discard_changed_admission(std::uint64_t source, const transport::JoinIdentity& identity) {
    const auto pending = admissions_.find(source);
    if (pending != admissions_.end() && !(pending->second.identity == identity)) {
      // Only called after a response's cookie and identity echo agree. A new proof or
      // handshake must receive its own verdict; a late result for the old request is ignored.
      admissions_.erase(pending);
    }
  }
  void prune_abandoned_admissions(double now, std::uint64_t current_source) {
    for (auto stale = admissions_.begin(); stale != admissions_.end();) {
      if (stale->first != current_source &&
          now - stale->second.started > kAdmissionTimeoutSeconds) {
        // Expired work must not reserve every slot and prevent queued peers from
        // reaching admission. Keep the current source long enough to send its refusal.
        stale = admissions_.erase(stale);
      } else {
        ++stale;
      }
    }
  }
  static bool queued_before(const QueueEntry& a, const QueueEntry& b) {
    if (a.priority != b.priority) {
      return a.priority > b.priority;
    }
    if (a.since != b.since) {
      return a.since < b.since;
    }
    return a.id < b.id;
  }
  // The place of the entry of `source` (1 is next), 0 when it does not wait.
  std::uint16_t queue_place(std::uint64_t source) const {
    auto own = queue_.find(source);
    if (own == queue_.end()) {
      return 0;
    }
    std::size_t place = 1;
    for (const auto& [other, entry] : queue_) {
      if (other != source && queued_before(entry, own->second)) {
        place += 1;
      }
    }
    return std::uint16_t(std::min<std::size_t>(place, 0xFFFF));
  }
  void leave_queue(std::uint64_t source, const char* reason) {
    auto entry = queue_.find(source);
    if (entry == queue_.end()) {
      return;
    }
    if (scripts_ && scripts_->alive()) {
      scripts_->send(Value(Value::Map{{"op", Value("joinUnqueued")},
                                      {"id", Value(std::int64_t(entry->second.id))},
                                      {"reason", Value(std::string(reason))}}));
    }
    queue_.erase(entry);
  }
  void send_wait_status(const sockaddr_in& from, std::uint64_t nonce, std::uint16_t place,
                        double since, double now, const std::string& line,
                        const std::vector<transport::WaitField>& fields) {
    transport::WaitStatus status;
    status.echo = nonce;
    status.place = place;
    status.waited_seconds = std::uint32_t(std::max(0.0, now - since));
    status.line = line;
    status.fields = fields;
    socket_.send(from, transport::encode_wait_status(status));
  }
  // True while the response of `source` must wait in the queue (answered with its status).
  bool waits_in_queue(std::uint64_t source, const sockaddr_in& from, const Datagram& d,
                      double now) {
    const auto nonce = gate_.verified_response(source, d);
    if (!nonce) {
      return false;
    }
    if (scripts_ && identity_of(source).echo != *nonce) {
      // Identity and response are separate datagrams; wait for a matching pair.
      return true;
    }
    const transport::JoinIdentity identity = identity_of(source);
    discard_changed_admission(source, identity);
    prune_abandoned_admissions(now, source);
    for (auto stale = queue_.begin(); stale != queue_.end();) {
      if (now - stale->second.heard > kQueueSilenceSeconds) {
        const std::uint64_t gone = stale->first;
        ++stale;
        leave_queue(gone, "left");
      } else {
        ++stale;
      }
    }
    auto entry = queue_.find(source);
    if (entry != queue_.end() && (entry->second.identity.echo != identity.echo ||
                                  entry->second.identity.nickname != identity.nickname ||
                                  entry->second.identity.steam_id != identity.steam_id)) {
      // Queue decisions belong to the handshake and identity shown to playerQueued.
      // Renewing only the licence pass preserves the queue position; changing the
      // handshake or claimed queue identity must not inherit priority or rejection.
      leave_queue(source, "left");
      entry = queue_.end();
    }
    if (entry != queue_.end() && entry->second.rejected) {
      Datagram text;
      const bool has_text = !entry->second.reason.empty() &&
                            gate_.refusal_text(source, d, entry->second.reason, text);
      Datagram reply;
      if (gate_.refuse(source, d, transport::RefusalReason::Rejected, kRefusalRetryAfterMs,
                       reply)) {
        if (has_text) {
          socket_.send(from, text);
        }
        socket_.send(from, reply);
      }
      queue_.erase(entry);
      return true;
    }
    const bool admitting = admissions_.count(source) != 0;
    const bool full =
        members_.size() + admissions_.size() - (admitting ? 1 : 0) >= settings_.max_peers;
    if (!full && entry == queue_.end() && (queue_.empty() || admitting)) {
      return false;
    }
    if (!full && entry != queue_.end() && queue_place(source) == 1) {
      leave_queue(source, "admitted");
      return false;
    }
    if (entry == queue_.end()) {
      if (queue_.size() >= kMaxQueue) {
        counters_.handshakes_refused_at_capacity += 1;
        refuse_over_capacity(source, from, d, now);
        return true;
      }
      QueueEntry fresh;
      fresh.id = ++next_queue_id_;
      fresh.since = now;
      fresh.identity = identity;
      entry = queue_.emplace(source, fresh).first;
      char address[INET_ADDRSTRLEN] = {};
      inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));
      std::cout << "JOIN queued id=" << fresh.id << " place=" << queue_place(source) << std::endl;
      if (scripts_ && scripts_->alive()) {
        scripts_->send(
            Value(Value::Map{{"op", Value("joinQueued")},
                             {"id", Value(std::int64_t(fresh.id))},
                             {"ip", Value(std::string(address))},
                             {"nickname", Value(identity.nickname)},
                             {"steamId", Value(identity.steam_id)},
                             {"discordId", Value(identity.discord_id)},
                             {"deviceId", Value(identity.hardware_id)},
                             {"licenceBinding", Value(transport::licence_binding(*nonce))},
                             {"licence", Value(identity.licence)}}));
      }
    }
    entry->second.heard = now;
    send_wait_status(from, *nonce, queue_place(source), entry->second.since, now,
                     entry->second.line, entry->second.fields);
    return true;
  }
  QueueEntry* queue_entry(std::uint32_t id) {
    for (auto& [source, entry] : queue_) {
      (void)source;
      if (entry.id == id) {
        return &entry;
      }
    }
    return nullptr;
  }
  // A join request to the resources (playerConnecting): their licence gate, password, bans and
  // handlers decide it. `bot`: an admitted bot, which brings no licence pass.
  void send_player_connecting(const sockaddr_in& from, std::uint64_t nonce,
                              const transport::JoinIdentity& identity, std::uint32_t request,
                              bool bot) {
    char address[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));
    scripts_->send(Value(Value::Map{{"op", Value("playerConnecting")},
                                    {"request", Value(std::int64_t(request))},
                                    {"ip", Value(std::string(address))},
                                    {"nickname", Value(identity.nickname)},
                                    {"steamId", Value(identity.steam_id)},
                                    {"discordId", Value(identity.discord_id)},
                                    {"deviceId", Value(identity.hardware_id)},
                                    {"licenceBinding", Value(transport::licence_binding(nonce))},
                                    {"licence", Value(identity.licence)},
                                    {"passwordProof", Value(identity.password_proof)},
                                    {"joinToken", Value(identity.join_token)},
                                    {"bot", Value(bot)}}));
  }
  bool admission_allows(std::uint64_t source, const sockaddr_in& from, const Datagram& d,
                        double now) {
    if (licence_listing_pending_)
      return false;  // public registration must supply the verifier's audience first
    if (!scripts_)
      return !licence_listed_;  // low-level headless fixtures; the server executable latches true
    if (!scripts_->alive())
      return false;  // a crashed verifier must never disable admission checks
    const auto verified_nonce = gate_.verified_response(source, d);
    const transport::JoinIdentity identity = identity_of(source);
    if (!verified_nonce || identity.echo != *verified_nonce)
      return false;
    discard_changed_admission(source, identity);
    auto pending = admissions_.find(source);
    if (pending == admissions_.end()) {
      PendingAdmission admission;
      admission.request = ++next_admission_request_;
      admission.started = now;
      admission.identity = identity;
      // A bot (kcds-bots) comes in only on a development server that asked for bots; any other
      // server turns it away here, before the resources hear of it.
      const integration::BotAdmission bot = integration::bot_admission(
          identity, settings_.allow_bots, core::kDevelopmentHandshakeKey);
      const bool bot_refused = bot == integration::BotAdmission::RefusedNotAllowed ||
                               bot == integration::BotAdmission::RefusedReleaseKey;
      if (bot_refused) {
        admission.decided = true;
        admission.allowed = false;
        admission.reason = std::string(integration::kBotRefusalText);
        std::cout << integration::bot_refusal_log(bot) << std::endl;
      }
      pending = admissions_.emplace(source, admission).first;
      if (!bot_refused) {
        send_player_connecting(from, *verified_nonce, identity, admission.request,
                               bot == integration::BotAdmission::Allowed);
        return false;
      }
    }
    if (!pending->second.decided) {
      // The waiting player sees the resources' line (connection.update).
      if (const auto nonce = gate_.verified_response(source, d)) {
        send_wait_status(from, *nonce, 0, pending->second.started, now, pending->second.line, {});
      }
    }
    if (now - pending->second.started > kAdmissionTimeoutSeconds) {
      pending->second.decided = true;
      pending->second.allowed = false;
      pending->second.timed_out = true;
      std::cout << "JOIN refused: admission expired before the handshake completed" << std::endl;
    }
    if (!pending->second.decided)
      return false;
    const bool allowed = pending->second.allowed;
    auto reason = transport::RefusalReason::Rejected;
    if (pending->second.timed_out) {
      reason = transport::RefusalReason::AdmissionTimeout;
    } else if (pending->second.password) {
      reason = transport::RefusalReason::PasswordRequired;
    } else if (pending->second.banned) {
      reason = transport::RefusalReason::Banned;
    } else if (pending->second.discord) {
      reason = transport::RefusalReason::DiscordRequired;
    }
    if (!allowed) {
      // The handler's reason goes first, so it is there when the refusal ends the attempt. Both
      // answer one verified response and share one token of the refusal budget.
      Datagram text;
      const bool has_text = !pending->second.reason.empty() &&
                            gate_.refusal_text(source, d, pending->second.reason, text);
      Datagram reply;
      if (gate_.refuse(source, d, reason, kRefusalRetryAfterMs, reply) &&
          refusal_budget_.try_take(now)) {
        if (has_text)
          socket_.send(from, text);
        socket_.send(from, reply);
      }
    }
    if (allowed) {
      // The joined player's entity is told to the scripts with this request (playerAdmitted),
      // so the identity they admitted becomes the player's.
      if (admitted_requests_.size() >= kMaxIdentities) {
        admitted_requests_.clear();
      }
      admitted_requests_[source] = pending->second.request;
    }
    admissions_.erase(pending);
    return allowed;
  }
  void decide_admission(std::uint32_t request, bool allowed, const std::string& reason,
                        bool password = false, bool banned = false, bool discord = false) {
    for (auto& [source, admission] : admissions_) {
      (void)source;
      if (admission.request == request && !admission.decided) {
        admission.decided = true;
        admission.allowed = allowed;
        admission.password = !allowed && password;
        admission.banned = !allowed && banned;
        admission.discord = !allowed && discord;
        if (!allowed)
          admission.reason = transport::sanitize_refusal_text(reason);
        std::cout << "JOIN admission request=" << request << " allowed=" << allowed;
        if (!admission.reason.empty())
          std::cout << " reason=\"" << admission.reason << '"';
        std::cout << std::endl;
        return;
      }
    }
  }
  void refuse_over_capacity(std::uint64_t source, const sockaddr_in& from, const Datagram& d,
                            double now) {
    Datagram reply;
    if (!gate_.refuse(source, d, transport::RefusalReason::ServerFull, kRefusalRetryAfterMs,
                      reply)) {
      return;
    }
    if (!refusal_budget_.try_take(now)) {
      counters_.refusals_rate_limited += 1;
      return;
    }
    if (socket_.send(from, reply)) {
      counters_.refusals_sent += 1;
    }
  }
  // A CONNECT_REQUEST of another KCDS build is told this server's version: the
  // refusal, then its text. The request is unauthenticated and the answer larger, so an address
  // hears it at most once every kOtherBuildAnswerSeconds, within the global refusal budget. True
  // when the request was another build's (it is never passed on to the gate).
  bool answer_other_build(std::uint64_t source, const sockaddr_in& from, const Datagram& d,
                          double now) {
#ifdef KCDS_SERVER_VERSION
    Datagram refusal;
    Datagram text;
    if (!gate_.refuse_other_build(d, KCDS_SERVER_VERSION, refusal, text)) {
      return false;
    }
    const auto last = other_build_answered_.find(source);
    if (last != other_build_answered_.end() && now - last->second < kOtherBuildAnswerSeconds) {
      return true;
    }
    if (!refusal_budget_.try_take(now)) {
      counters_.refusals_rate_limited += 1;
      return true;
    }
    if (other_build_answered_.size() >= kOtherBuildAddresses) {
      other_build_answered_.clear();
    }
    other_build_answered_[source] = now;
    if (socket_.send(from, refusal)) {
      counters_.refusals_sent += 1;
      socket_.send(from, text);
    }
    return true;
#else
    (void)source;
    (void)from;
    (void)d;
    (void)now;
    return false;
#endif
  }
  // A { x, y, z } field into three floats.
  static void read_point(const Value& v, float point[3]) {
    point[0] = float(number(v, "x"));
    point[1] = float(number(v, "y"));
    point[2] = float(number(v, "z"));
  }
  static Value point_value(const float point[3]) {
    return vector_value(core::Vec3{point[0], point[1], point[2]});
  }
  // One thing a player's game met along a ray, as the resource host reads it: the
  // level object a level edit finds it by in "lo".
  static Value ray_hit_value(const integration::WorldRayHit& hit) {
    static constexpr const char* kHitKinds[] = {"static", "terrain", "entity", "vegetation",
                                                "brush"};
    Value::Map entry{{"position", point_value(hit.position)},
                     {"normal", point_value(hit.normal)},
                     {"distance", Value(double(hit.distance))},
                     {"terrain", Value(hit.terrain)},
                     {"entity", Value(std::int64_t(hit.entity))},
                     {"surface", Value(hit.surface)},
                     {"kind", Value(kHitKinds[std::size_t(hit.kind)])},
                     {"model", Value(hit.model)},
                     {"guid", Value(hit.guid)},
                     {"name", Value(hit.name)},
                     {"cls", Value(hit.entity_class)}};
    if (hit.object != integration::RayObject::None) {
      const bool brush = hit.object == integration::RayObject::Brush;
      entry.emplace_back(
          "lo",
          Value(Value::Map{{"kind", Value(brush ? "brush" : "entity")},
                           {"name", Value(brush ? hit.model : hit.name)},
                           {"guid", Value(hit.guid)},
                           {"cls", Value(hit.entity_class)},
                           {"at", point_value(hit.object_at)},
                           {"rot", Value(Value::Array{Value(double(hit.object_rotation[0])),
                                                      Value(double(hit.object_rotation[1])),
                                                      Value(double(hit.object_rotation[2])),
                                                      Value(double(hit.object_rotation[3]))})},
                           {"scale", point_value(hit.object_scale)}}));
    }
    return Value(std::move(entry));
  }
  static Value vector_value(core::Vec3 v) {
    return Value(Value::Map{
        {"x", Value(double(v.x))}, {"y", Value(double(v.y))}, {"z", Value(double(v.z))}});
  }
  static Value rotation_value(core::Quat q) {
    return Value(Value::Map{{"w", Value(double(q.w))},
                            {"x", Value(double(q.x))},
                            {"y", Value(double(q.y))},
                            {"z", Value(double(q.z))}});
  }
  static const Value* field(const Value& v, const char* key) {
    if (v.tag() != sdk::Tag::Map)
      return nullptr;
    for (const auto& [name, x] : v.as_map())
      if (name == key)
        return &x;
    return nullptr;
  }
  static double number(const Value& v, const char* key, double fallback = 0) {
    auto x = field(v, key);
    if (!x)
      return fallback;
    if (x->tag() == sdk::Tag::Double)
      return x->as_double();
    if (x->tag() == sdk::Tag::Int)
      return double(x->as_int());
    throw std::runtime_error("Resource numeric field expected");
  }
  // A { x, y, z } field as three floats; `fallback` for the parts that are missing.
  static std::array<float, 3> floats3(const Value& v, const char* key, float fallback = 0.0f) {
    const sdk::Value* part = field(v, key);
    if (!part) {
      return {fallback, fallback, fallback};
    }
    return {float(number(*part, "x", fallback)), float(number(*part, "y", fallback)),
            float(number(*part, "z", fallback))};
  }
  // A quaternion { x, y, z, w } field; no rotation when missing.
  static std::array<float, 4> quaternion4(const Value& v, const char* key) {
    const sdk::Value* part = field(v, key);
    if (!part) {
      return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return {float(number(*part, "x", 0)), float(number(*part, "y", 0)),
            float(number(*part, "z", 0)), float(number(*part, "w", 1))};
  }
  static integration::PropOrder prop_order_of(const Value& e, integration::PropKind kind) {
    integration::PropOrder order;
    order.kind = kind;
    order.id = std::uint32_t(number(e, "id"));
    order.model = text(e, "model");
    order.position = floats3(e, "position");
    order.rotation = quaternion4(e, "rotation");
    order.scale = floats3(e, "scale", 1.0f);
    const std::string physics = text(e, "physics");
    order.physics = physics == "rigid"    ? integration::PropPhysics::Rigid
                    : physics == "static" ? integration::PropPhysics::Static
                    : physics == "wall"   ? integration::PropPhysics::Wall
                                          : integration::PropPhysics::None;
    order.dimension = std::uint32_t(number(e, "dimension"));
    order.mass = float(number(e, "mass", 0));
    order.simulate = flag_of(e, "simulate");
    const std::string push = text(e, "push");
    order.push = push == "impulse"    ? integration::PropPush::Impulse
                 : push == "velocity" ? integration::PropPush::Velocity
                 : push == "sleep"    ? integration::PropPush::Sleep
                                      : integration::PropPush::Wake;
    order.linear = floats3(e, "linear");
    order.angular = floats3(e, "angular");
    order.has_point = flag_of(e, "hasPoint");
    order.point = floats3(e, "point");
    // A prop on a body.
    const double body = number(e, "body", 0);
    order.body = body >= 1 && body <= 4294967295.0 ? std::uint32_t(body) : 0;
    order.joint = text(e, "joint");
    // A move that glides (Prop.glideTo).
    const double glide = number(e, "ms", 0);
    if (kind == integration::PropKind::Move && glide >= 1 &&
        glide <= integration::kMaxPropGlideMs) {
      order.glide_ms = std::uint32_t(glide);
      order.glide_arc = float(number(e, "arc", 0));
    }
    return order;
  }
  // The CART kind of a server-to-client cart op, or none.
  static std::optional<integration::CartKind> cart_kind_of(const std::string& op) {
    using integration::CartKind;
    static const std::pair<const char*, CartKind> kinds[] = {
        {"cartSpawn", CartKind::Spawn},         {"cartDestroy", CartKind::Destroy},
        {"cartSeat", CartKind::Seat},           {"cartLeave", CartKind::Leave},
        {"cartDeny", CartKind::Deny},           {"cartTeleport", CartKind::Teleport},
        {"cartSpeed", CartKind::Speed},         {"cartPose", CartKind::Pose},
        {"cartLoad", CartKind::Load},           {"cartSettled", CartKind::Settled},
        {"cartClimbOff", CartKind::ClimbOff},   {"cartReinsTaken", CartKind::ReinsTaken},
        {"cartReinsGone", CartKind::ReinsGone}, {"cartClimbOn", CartKind::ClimbOn},
        {"cartScootOff", CartKind::ScootOff},
    };
    for (const auto& [name, kind] : kinds) {
      if (op == name) {
        return kind;
      }
    }
    return std::nullopt;
  }
  // A { x, y, z } and a quaternion as the bridge's maps.
  static Value point_value(const std::array<float, 3>& p) {
    return Value(Value::Map{
        {"x", Value(double(p[0]))}, {"y", Value(double(p[1]))}, {"z", Value(double(p[2]))}});
  }
  static Value quaternion_value(const std::array<float, 4>& q) {
    return Value(Value::Map{{"x", Value(double(q[0]))},
                            {"y", Value(double(q[1]))},
                            {"z", Value(double(q[2]))},
                            {"w", Value(double(q[3]))}});
  }
  static integration::VfxTuning vfx_tuning_of(const Value& e) {
    integration::VfxTuning tuning;
    tuning.scale = float(number(e, "scale", 1.0));
    tuning.count_scale = float(number(e, "countScale", 1.0));
    tuning.speed_scale = float(number(e, "speedScale", 1.0));
    tuning.time_scale = float(number(e, "timeScale", 1.0));
    tuning.strength = float(number(e, "strength", -1.0));
    tuning.pulse_period = float(number(e, "pulsePeriod", 0.0));
    return tuning;
  }
  static integration::VfxOrder vfx_order_of(const Value& e, integration::VfxKind kind) {
    integration::VfxOrder order;
    order.kind = kind;
    order.effect = text(e, "effect");
    order.rotation = quaternion4(e, "rotation");
    order.dimension = std::uint32_t(number(e, "dimension"));
    order.duration_ms = std::uint32_t(number(e, "durationMs"));
    order.tuning = vfx_tuning_of(e);
    const sdk::Value* prime = field(e, "prime");
    order.prime = prime && prime->tag() == sdk::Tag::Bool && prime->as_bool();
    return order;
  }
  // The meshes of a preview op (`models`) with their `offsets` and `rotations`; a
  // missing offset is none, a missing rotation the identity.
  static std::vector<integration::PreviewPiece> preview_pieces_of(const Value& e) {
    std::vector<integration::PreviewPiece> pieces;
    const sdk::Value* models = field(e, "models");
    if (!models || models->tag() != sdk::Tag::Array) {
      return pieces;
    }
    const sdk::Value* offsets = field(e, "offsets");
    const sdk::Value* rotations = field(e, "rotations");
    const auto item = [](const sdk::Value* list, std::size_t index) -> const sdk::Value* {
      if (!list || list->tag() != sdk::Tag::Array || index >= list->as_array().size()) {
        return nullptr;
      }
      return &list->as_array()[index];
    };
    const auto& names = models->as_array();
    for (std::size_t i = 0; i < names.size() && i < integration::kMaxPreviewPieces; ++i) {
      if (names[i].tag() != sdk::Tag::Str) {
        return {};
      }
      integration::PreviewPiece piece;
      piece.model = names[i].as_str();
      if (const sdk::Value* offset = item(offsets, i)) {
        piece.offset = {float(number(*offset, "x")), float(number(*offset, "y")),
                        float(number(*offset, "z"))};
      }
      if (const sdk::Value* rotation = item(rotations, i)) {
        piece.rotation = {float(number(*rotation, "x")), float(number(*rotation, "y")),
                          float(number(*rotation, "z")), float(number(*rotation, "w", 1))};
      }
      pieces.push_back(std::move(piece));
    }
    return pieces;
  }
  // The placerReport op of a client's PLACER_REPORT.
  static Value placer_report_value(core::EntityId player, const integration::PlacerReport& r) {
    using integration::PlacerEvent;
    const char* event = r.event == PlacerEvent::Move      ? "move"
                        : r.event == PlacerEvent::Confirm ? "confirm"
                        : r.event == PlacerEvent::Cancel  ? "cancel"
                                                          : "end";
    Value::Map message{{"op", Value("placerReport")},
                       {"id", Value(std::int64_t(player))},
                       {"session", Value(std::int64_t(r.session))},
                       {"event", Value(event)},
                       {"valid", Value(r.valid)}};
    if (r.event == PlacerEvent::Move || r.event == PlacerEvent::Confirm) {
      const core::Quat rotation{r.rotation[0], r.rotation[1], r.rotation[2], r.rotation[3]};
      message.emplace_back(
          "pose",
          Value(Value::Map{
              {"position", vector_value(core::Vec3{r.position[0], r.position[1], r.position[2]})},
              {"rotation", rotation_value(rotation)},
              {"normal", vector_value(core::Vec3{r.normal[0], r.normal[1], r.normal[2]})},
              {"surface", Value("")},
              {"distance", Value(double(r.distance))},
              {"onSurface", Value(r.on_surface)}}));
    }
    return Value(std::move(message));
  }
  // A { r, g, b, a } field; not present when missing or null.
  static integration::PreviewTint preview_tint_of(const Value& e, const char* key) {
    integration::PreviewTint tint;
    const sdk::Value* value = field(e, key);
    if (!value || value->tag() != sdk::Tag::Map) {
      return tint;
    }
    tint.present = true;
    tint.rgba = {float(number(*value, "r", 1)), float(number(*value, "g", 1)),
                 float(number(*value, "b", 1)), float(number(*value, "a", 1))};
    return tint;
  }
  static integration::GhostOrder ghost_order_of(const Value& e, integration::GhostKind kind) {
    integration::GhostOrder order;
    order.kind = kind;
    order.ghost = std::uint32_t(number(e, "ghost"));
    order.pieces = preview_pieces_of(e);
    order.position = floats3(e, "position");
    order.rotation = quaternion4(e, "rotation");
    order.scale = float(number(e, "scale", 1.0));
    if (field(e, "scaleAxes")) {
      order.axes = floats3(e, "scaleAxes");
    }
    order.tint = preview_tint_of(e, "tint");
    order.hidden = flag_of(e, "hidden");
    return order;
  }
  static integration::PlacerOrder placer_begin_of(const Value& e) {
    integration::PlacerOrder order;
    order.kind = integration::PlacerKind::Begin;
    order.session = std::uint32_t(number(e, "session"));
    order.pieces = preview_pieces_of(e);
    order.scale = float(number(e, "scale", 1.0));
    const integration::PreviewTint valid = preview_tint_of(e, "validTint");
    const integration::PreviewTint invalid = preview_tint_of(e, "invalidTint");
    if (valid.present) {
      order.valid_tint = valid.rgba;
    }
    if (invalid.present) {
      order.invalid_tint = invalid.rgba;
    }
    order.range = float(number(e, "range", order.range));
    order.fallback_distance = float(number(e, "fallbackDistance", order.fallback_distance));
    order.vertical_offset = float(number(e, "verticalOffset", 0));
    order.yaw = float(number(e, "yaw", 0));
    const sdk::Value* rules = field(e, "rules");
    const Value no_rules{Value::Map{}};
    const Value& rule = rules && rules->tag() == sdk::Tag::Map ? *rules : no_rules;
    order.max_slope = float(number(rule, "maxSlope", -1));
    order.min_distance = float(number(rule, "minDistance", -1));
    order.max_distance = float(number(rule, "maxDistance", -1));
    const std::pair<const char*, std::uint8_t> flags[] = {
        {"alignToSurface", integration::kPlacerAlignToSurface},
        {"faceAwayFromPlayer", integration::kPlacerFaceAwayFromPlayer},
        {"keepOpen", integration::kPlacerKeepOpen},
        {"reportMoves", integration::kPlacerReportMoves},
    };
    for (const auto& [name, bit] : flags) {
      if (flag_of(e, name)) {
        order.flags |= bit;
      }
    }
    if (flag_of(rule, "requireSurface")) {
      order.flags |= integration::kPlacerRequireSurface;
    }
    return order;
  }
  static integration::GizmoOrder gizmo_draw_of(const Value& e) {
    integration::GizmoOrder order;
    order.kind = integration::GizmoKind::Draw;
    order.gizmo = std::uint32_t(number(e, "gizmo"));
    const std::string shape = text(e, "shape");
    if (shape == "line") {
      order.shape = integration::GizmoShape::Line;
      order.extent = floats3(e, "end");
    } else if (shape == "sphere") {
      order.shape = integration::GizmoShape::Sphere;
      order.size = float(number(e, "radius", 1.0));
    } else if (shape == "box") {
      order.shape = integration::GizmoShape::Box;
      order.extent = floats3(e, "size", 0.5f);
      order.rotation = quaternion4(e, "rotation");
    } else if (shape == "text") {
      order.shape = integration::GizmoShape::Text;
      order.size = float(number(e, "textSize", 1.5));
      order.text = text(e, "text");
    } else {
      order.shape = integration::GizmoShape(0);
    }
    order.position = floats3(e, "position");
    const integration::PreviewTint color = preview_tint_of(e, "color");
    if (color.present) {
      order.color = color.rgba;
    }
    order.duration_ms = std::uint32_t(number(e, "durationMs", 0));
    return order;
  }
  // The players a gizmo op names (`players`), or the single 0 (every ready player) without it.
  static std::vector<core::EntityId> gizmo_players_of(const Value& e) {
    const sdk::Value* players = field(e, "players");
    if (!players || players->tag() != sdk::Tag::Array) {
      return {core::EntityId(0)};
    }
    std::vector<core::EntityId> ids;
    for (const sdk::Value& player : players->as_array()) {
      if (player.tag() == sdk::Tag::Int || player.tag() == sdk::Tag::Double) {
        const double id =
            player.tag() == sdk::Tag::Int ? double(player.as_int()) : player.as_double();
        if (exact_uint32(id) && id > 0) {
          ids.push_back(core::EntityId(id));
        }
      }
    }
    return ids;
  }
  static std::string text(const Value& v, const char* key) {
    auto x = field(v, key);
    if (!x)
      return {};
    if (x->tag() != sdk::Tag::Str)
      throw std::runtime_error("Resource string field expected");
    return x->as_str();
  }
  // True for a finite, integral value inside the uint32 identifier domain.
  static bool exact_uint32(double value) {
    return std::isfinite(value) && std::floor(value) == value && value >= 0 &&
           value <= double(0xffffffffu);
  }
  bool connected_avatar(core::EntityId id) const {
    if (id == core::kInvalidEntity)
      return false;
    return std::any_of(members_.begin(), members_.end(),
                       [&](const auto& entry) { return entry.second.avatar == id; });
  }
  // A horse spawned by a resource; its script id is its network id.
  bool resource_horse(core::EntityId id) const {
    auto found = horses_.find(std::uint32_t(id));
    return found != horses_.end() && found->second == id;
  }
  // An NPC spawned by a resource (Npc.create); its script id is its network id.
  bool resource_npc(core::EntityId id) const {
    auto found = npcs_.find(std::uint32_t(id));
    return found != npcs_.end() && found->second == id;
  }
  // Player state the resource host last received; equal reports are skipped.
  struct PlayerReport {
    std::uint16_t player_index = 0;
    bool ready = false;
    bool gameplay_ready = false;
    std::uint32_t dimension = 0;
    core::Vec3 position{};
    core::Quat rotation{};
    integration::Appearance appearance;
    bool visible = true;
    // Whether the helmet's visor is down.
    bool visor_closed = false;
    std::string disguise;
    // The station the player works at: the NetworkPlayer field "stationAction"'s kind and
    // station words ("smith S1997.1,2498.7,115.0"), "" at none.
    std::string station;
    std::string nickname;
    std::optional<integration::PlayerVitals> vitals;
    std::optional<integration::PlayerStatsSample> stats;
    std::optional<integration::PlayerAbilities> abilities;
    core::Vec3 velocity{};
    core::Vec3 look_direction{};
    integration::PlayerPosture posture;
    std::string outfit;    // the owner's "outfit" field
    std::string hands;     // the owner's "hands" field
    std::string carry;     // the owner's "carry" field
    std::string language;  // the owner's "language" field
    std::string grime;     // the owner's "grime" field
    // Whether the player holds a seat (the owner's "seats" field is not empty): sits on a
    // chair, a bench or a dice table's stool.
    bool seated = false;
    friend bool operator==(const PlayerReport& a, const PlayerReport& b) {
      return a.player_index == b.player_index && a.ready == b.ready &&
             a.gameplay_ready == b.gameplay_ready && a.dimension == b.dimension &&
             a.position == b.position && a.rotation == b.rotation && a.appearance == b.appearance &&
             a.visible == b.visible && a.visor_closed == b.visor_closed &&
             a.disguise == b.disguise && a.nickname == b.nickname && a.vitals == b.vitals &&
             a.stats == b.stats && a.abilities == b.abilities && a.velocity == b.velocity &&
             a.look_direction == b.look_direction && a.posture == b.posture &&
             a.outfit == b.outfit && a.hands == b.hands && a.carry == b.carry &&
             a.language == b.language && a.grime == b.grime && a.seated == b.seated;
    }
  };
  struct HorseReport {
    std::uint32_t dimension = 0;
    core::Vec3 position{};
    core::Quat rotation{};
    std::int64_t rider = 0;
    friend bool operator==(const HorseReport& a, const HorseReport& b) {
      return a.dimension == b.dimension && a.position == b.position && a.rotation == b.rotation &&
             a.rider == b.rider;
    }
  };
  // What the resource host last heard of a resource NPC (npcState); its simulator travels in
  // npcSimulator at every change instead.
  struct NpcReport {
    std::uint32_t dimension = 0;
    core::Vec3 position{};
    core::Quat rotation{};
    core::Vec3 velocity{};
    friend bool operator==(const NpcReport& a, const NpcReport& b) {
      return a.dimension == b.dimension && a.position == b.position && a.rotation == b.rotation &&
             a.velocity == b.velocity;
    }
  };
  // Sends the player's state when it differs from what the resource host holds. Local SDK reads
  // stay local; this keeps their replica current without a full report every 100 ms.
  void report_player(core::EntityId id) {
    report_player(id, server_.player_ready(id), server_.player_gameplay_ready(id));
  }
  // The same for a known connection, without scanning every client.
  void report_peer_player(core::EntityId id, core::PeerId peer) {
    report_player(id, server_.peer_world_ready(peer), server_.peer_gameplay_ready(peer));
  }
  void report_player(core::EntityId id, bool ready, bool gameplay_ready) {
    if (!scripts_)
      return;
    auto p = server_.world().read(id, Position);
    auto q = server_.world().read(id, Rotation);
    const auto* slot = server_.world().slot(id);
    if (!p || !q || !slot)
      return;
    PlayerReport report;
    report.player_index = player_slots_.slot_of(id);
    report.nickname = nickname_of(id);
    report.ready = ready;
    report.gameplay_ready = gameplay_ready;
    report.dimension = slot->dimension;
    report.position = std::get<core::Vec3>(*p);
    report.rotation = std::get<core::Quat>(*q);
    // Until the owner reports its appearance, the player has the default body.
    if (auto appearance = server_.world().read(id, Appearance)) {
      integration::parse_appearance_field(std::get<std::string>(*appearance), report.appearance);
    }
    if (auto visible = server_.world().read(id, BodyVisible)) {
      report.visible = std::get<bool>(*visible);
    }
    if (auto visor = server_.world().read(id, VisorClosed)) {
      report.visor_closed = std::get<bool>(*visor);
    }
    if (auto disguise = server_.world().read(id, Disguise)) {
      report.disguise = std::get<std::string>(*disguise);
    }
    if (auto action = server_.world().read(id, StationAction)) {
      const std::string& field = std::get<std::string>(*action);
      const std::size_t first = field.find(' ');
      const std::size_t second = first == std::string::npos ? first : field.find(' ', first + 1);
      report.station = field.substr(0, second);
    }
    report.vitals = vitals_of(id);
    report.stats = stats_of(id);
    report.abilities = abilities_of(id);
    report.velocity = vector_of(id, Velocity);
    report.look_direction = vector_of(id, LookDirection);
    report.posture = posture_of(id);
    if (auto outfit = server_.world().read(id, Outfit)) {
      report.outfit = std::get<std::string>(*outfit);
    }
    if (auto hands = server_.world().read(id, Hands)) {
      report.hands = std::get<std::string>(*hands);
    }
    if (auto carry = server_.world().read(id, Carry)) {
      report.carry = std::get<std::string>(*carry);
    }
    if (auto language = server_.world().read(id, Language)) {
      report.language = std::get<std::string>(*language);
    }
    if (auto grime = server_.world().read(id, Grime)) {
      report.grime = std::get<std::string>(*grime);
    }
    if (auto seats = server_.world().read(id, Seats)) {
      report.seated = !std::get<std::string>(*seats).empty();
    }
    if (!player_reports_.changed(id, report))
      return;
    scripts_->send(
        Value(Value::Map{{"op", Value("player")},
                         {"id", Value(std::int64_t(id))},
                         {"playerIndex", Value(std::int64_t(report.player_index))},
                         {"nickname", Value(report.nickname)},
                         {"ready", Value(report.ready)},
                         {"gameplayReady", Value(report.gameplay_ready)},
                         {"dimension", Value(std::int64_t(report.dimension))},
                         {"position", vector_value(report.position)},
                         {"rotation", rotation_value(report.rotation)},
                         {"appearance", appearance_value(report.appearance)},
                         {"visible", Value(report.visible)},
                         {"visorDown", Value(report.visor_closed)},
                         {"disguise", Value(report.disguise)},
                         {"station", Value(report.station)},
                         {"vitals", vitals_value(report.vitals)},
                         {"stats", stats_value(report.stats)},
                         {"scaledStats", relative_stats_value(report.stats)},
                         {"abilities", abilities_value(report.abilities)},
                         {"velocity", vector_value(report.velocity)},
                         {"gaze", vector_value(report.look_direction)},
                         {"inAir", Value(report.posture.in_air)},
                         {"crouched", Value(report.posture.crouched)},
                         {"stance", Value(std::int64_t(report.posture.stance))},
                         {"guard", Value(report.posture.guard)},
                         {"guardUp", Value(report.posture.raised)},
                         {"fightZone", Value(std::int64_t(report.posture.combat_zone))},
                         {"physicsProfile", Value(std::int64_t(report.posture.physics_profile))},
                         {"equipment", equipment_value(report.outfit)},
                         {"hands", hands_value(report.hands)},
                         {"carry", carry_value(report.carry)},
                         {"language", Value(report.language)},
                         {"grime", grime_value(report.grime)},
                         {"seated", Value(report.seated)}}));
    // The resource host hears a player's progression after its first report, then on change.
    if (!progression_reported_.count(id)) {
      progression_reported_.insert(id);
      report_progression(id);
    }
  }
  // Sends the player's progression to the resource host, which passes it to the resources and to
  // the clients' directories. Its own op, sent only when it changed: a few hundred perks have no
  // place in a report sent whenever the player moves.
  void report_progression(core::EntityId id) {
    auto found = progressions_.find(id);
    if (!scripts_ || found == progressions_.end())
      return;
    scripts_->send(Value(Value::Map{{"op", Value("playerProgression")},
                                    {"id", Value(std::int64_t(id))},
                                    {"progression", progression_value(found->second)}}));
  }
  // The progression the player's own client last reported; none until it
  // did.
  // { tracks: [[level, xp, points] per track, in Progression.tracks() order], mainPoints,
  //   perks: [GUID text] } with XP in the game's units.
  static Value progression_value(const integration::Progression& progression) {
    Value::Array tracks;
    for (const auto& track : progression.tracks) {
      tracks.emplace_back(Value::Array{Value(std::int64_t(track.level)),
                                       Value(double(track.xp) / integration::kXpUnits),
                                       Value(std::int64_t(track.points))});
    }
    Value::Array perks;
    for (const auto& perk : progression.perks)
      perks.emplace_back(integration::perk_id_text(perk));
    return Value(Value::Map{{"tracks", Value(std::move(tracks))},
                            {"mainPoints", Value(std::int64_t(progression.main_points))},
                            {"perks", Value(std::move(perks))}});
  }
  // The player's vitals and sampled stats as its own client last published them;
  // none until it did.
  std::optional<integration::PlayerVitals> vitals_of(core::EntityId id) const {
    integration::PlayerVitals vitals;
    auto field = server_.world().read(id, Vitals);
    if (!field || !integration::parse_vitals_field(std::get<std::string>(*field), vitals))
      return std::nullopt;
    return vitals;
  }
  std::optional<integration::PlayerStatsSample> stats_of(core::EntityId id) const {
    integration::PlayerStatsSample stats;
    auto field = server_.world().read(id, Stats);
    if (!field || !integration::parse_stats_field(std::get<std::string>(*field), stats))
      return std::nullopt;
    return stats;
  }
  // A Vec3 field of the entity, zero when it has none.
  core::Vec3 vector_of(core::EntityId id, core::FieldId field) const {
    auto value = server_.world().read(id, field);
    if (!value || !std::holds_alternative<core::Vec3>(*value))
      return {};
    return std::get<core::Vec3>(*value);
  }
  // How the player's body stands as its own client last published it; the defaults until then.
  integration::PlayerPosture posture_of(core::EntityId id) const {
    integration::PlayerPosture posture;
    auto value = server_.world().read(id, Posture);
    if (value && std::holds_alternative<std::int64_t>(*value))
      integration::parse_posture_field(std::get<std::int64_t>(*value), posture);
    return posture;
  }
  // The public id of the resource horse the player rides, 0 on foot.
  std::uint32_t horse_of(core::EntityId player) const {
    for (const auto& [script, native] : horses_) {
      auto rider = server_.world().read(native, 7);
      if (rider && std::holds_alternative<std::int64_t>(*rider) &&
          std::get<std::int64_t>(*rider) == std::int64_t(player))
        return script;
    }
    return 0;
  }
  std::optional<integration::PlayerAbilities> abilities_of(core::EntityId id) const {
    integration::PlayerAbilities abilities;
    auto field = server_.world().read(id, Abilities);
    if (!field || !integration::parse_abilities_field(std::get<std::string>(*field), abilities))
      return std::nullopt;
    return abilities;
  }
  // { maxHealth, ... maxHunger, level, strength, agility, vitality, skills: { fencing, ... },
  //   scaledSkills: { fencing, ... } } in native units, or null.
  static Value abilities_value(const std::optional<integration::PlayerAbilities>& abilities) {
    if (!abilities)
      return Value::null();
    Value::Map value;
    for (std::size_t i = 0; i < abilities->maxima.size(); ++i)
      value.emplace_back(integration::kMaximumNames[i],
                         Value(integration::stat_number(abilities->maxima[i])));
    value.emplace_back("level", Value(integration::stat_number(abilities->level)));
    for (std::size_t i = 0; i < abilities->attributes.size(); ++i)
      value.emplace_back(integration::kAttributeNames[i],
                         Value(integration::stat_number(abilities->attributes[i])));
    Value::Map skills;
    Value::Map relative;
    for (std::size_t i = 0; i < abilities->skills.size(); ++i) {
      skills.emplace_back(integration::kSkillNames[i],
                          Value(integration::stat_number(abilities->skills[i])));
      relative.emplace_back(integration::kSkillNames[i],
                            Value(integration::relative_number(abilities->relative_skills[i])));
    }
    value.emplace_back("skills", Value(std::move(skills)));
    value.emplace_back("scaledSkills", Value(std::move(relative)));
    return Value(std::move(value));
  }
  // { health, stamina, ... poisoning } in native units, or null.
  static Value vitals_value(const std::optional<integration::PlayerVitals>& vitals) {
    if (!vitals)
      return Value::null();
    Value::Map value;
    for (std::size_t i = 0; i < vitals->values.size(); ++i)
      value.emplace_back(integration::kVitalNames[i],
                         Value(integration::stat_number(vitals->values[i])));
    return Value(std::move(value));
  }
  // { charisma, ... morale } in native units, or null.
  static Value stats_value(const std::optional<integration::PlayerStatsSample>& stats) {
    if (!stats)
      return Value::null();
    Value::Map value;
    for (std::size_t i = 0; i < stats->derived.size(); ++i)
      value.emplace_back(integration::kDerivedNames[i],
                         Value(integration::stat_number(stats->derived[i])));
    return Value(std::move(value));
  }
  // { strength, agility, vitality } as the engine's relative values, or null.
  static Value relative_stats_value(const std::optional<integration::PlayerStatsSample>& stats) {
    if (!stats)
      return Value::null();
    Value::Map value;
    for (std::size_t i = 0; i < stats->relative.size(); ++i)
      value.emplace_back(integration::kRelativeNames[i],
                         Value(integration::relative_number(stats->relative[i])));
    return Value(std::move(value));
  }
  // An order of the resource host's progressionOrder message: { kind, track, xp (game units),
  // multipliers, level, count, perk (GUID text), reason (refusePerk), perks ([GUID text],
  // lockPerks), snapshot ({ tracks: [[level, xp, points]], mainPoints, perks: [GUID text] }),
  // notify (false: the game's notice of a level reached is held back) }.
  // False for anything malformed.
  static bool progression_order_of(const Value& e, integration::ProgressionOrder& order) {
    using Kind = integration::ProgressionOrder::Kind;
    const std::string kind = text(e, "kind");
    const auto track = number(e, "track", -1);
    if (track >= 0 && track <= 255)
      order.track = std::uint8_t(track);
    const auto* notify = field(e, "notify");
    order.notify = !(notify && notify->tag() == sdk::Tag::Bool && !notify->as_bool());
    if (kind == "addXp") {
      order.kind = Kind::AddXp;
      const double xp = number(e, "xp", 0) * integration::kXpUnits;
      if (!(xp >= 1) || xp > double(integration::kMaxOrderXp))
        return false;
      order.xp = std::uint32_t(std::llround(xp));
      const auto* multipliers = field(e, "multipliers");
      order.multipliers =
          multipliers && multipliers->tag() == sdk::Tag::Bool && multipliers->as_bool();
      return true;
    }
    if (kind == "setLevel") {
      order.kind = Kind::SetLevel;
      const auto level = number(e, "level", -1);
      if (level < 0 || level > integration::kLevelCap)
        return false;
      order.level = std::uint8_t(level);
      return true;
    }
    if (kind == "setTrack") {
      // The exact order: { track, level, xp } with xp towards the next level, 0 too.
      order.kind = Kind::SetTrack;
      order.notify = false;
      const auto level = number(e, "level", -1);
      const double xp = number(e, "xp", -1) * integration::kXpUnits;
      if (level < 0 || level > integration::kLevelCap || !(xp >= 0) ||
          xp > double(integration::kMaxOrderXp))
        return false;
      order.level = std::uint8_t(level);
      order.xp = std::uint32_t(std::llround(xp));
      return true;
    }
    if (kind == "addPerk" || kind == "removePerk" || kind == "learnPerk") {
      if (kind == "addPerk")
        order.kind = Kind::AddPerk;
      else if (kind == "removePerk")
        order.kind = Kind::RemovePerk;
      else
        order.kind = Kind::LearnPerk;
      return integration::parse_perk_id(text(e, "perk"), order.perk);
    }
    if (kind == "refusePerk") {
      order.kind = Kind::RefusePerk;
      static constexpr const char* kRefusals[] = {"points",  "level",  "parent", "exclusive",
                                                  "blocked", "script", "owned",  "hidden"};
      const std::string reason = text(e, "reason");
      for (std::uint8_t i = 0; i < integration::kPerkRefusalCount; ++i) {
        if (reason == kRefusals[i]) {
          order.refusal = integration::PerkRefusal(i);
          return integration::parse_perk_id(text(e, "perk"), order.perk);
        }
      }
      return false;
    }
    if (kind == "lockPerks") {
      order.kind = Kind::BlockPerks;
      const auto* perks = field(e, "perks");
      if (!perks || perks->tag() != sdk::Tag::Array ||
          perks->as_array().size() > integration::kMaxPerks)
        return false;
      for (const Value& perk : perks->as_array()) {
        integration::PerkId id{};
        if (perk.tag() != sdk::Tag::Str || !integration::parse_perk_id(perk.as_str(), id))
          return false;
        order.perks.push_back(id);
      }
      return true;
    }
    if (kind == "addPoints") {
      order.kind = Kind::AddPoints;
      const auto count = number(e, "count", 0);
      if (count < 1 || count > 0xFFFF)
        return false;
      order.count = std::uint16_t(count);
      return true;
    }
    if (kind == "respec") {
      order.kind = Kind::Respec;
      return true;
    }
    if (kind == "gameXp") {
      order.kind = Kind::NativeXp;
      const auto* enabled = field(e, "enabled");
      order.level = enabled && enabled->tag() == sdk::Tag::Bool && enabled->as_bool() ? 1 : 0;
      return true;
    }
    if (kind == "restore") {
      order.kind = Kind::Restore;
      // { exact: true }: written as it is, down as well as up.
      const auto* exact = field(e, "exact");
      order.exact = exact && exact->tag() == sdk::Tag::Bool && exact->as_bool();
      const auto* snapshot = field(e, "snapshot");
      return snapshot && snapshot_of(*snapshot, order.snapshot);
    }
    return false;
  }
  // Quickslots as the resource host writes them (inventories.cjs quickslotsWire): twelve row ids
  // ("" for an empty slot), the selected weapon slot and the selected item slot.
  static bool quickslots_of(const Value& list, integration::InventoryQuickslots& slots) {
    if (list.tag() != sdk::Tag::Array || list.as_array().size() != 14)
      return false;
    const auto& cells = list.as_array();
    auto id = [&](std::size_t index, std::string& out) {
      if (cells[index].tag() != sdk::Tag::Str ||
          cells[index].as_str().size() > integration::kMaxRowIdBytes)
        return false;
      out = cells[index].as_str();
      return true;
    };
    for (std::size_t slot = 0; slot < integration::kQuickslots; ++slot) {
      if (!id(slot * 2, slots.primary[slot]) || !id(slot * 2 + 1, slots.secondary[slot]) ||
          !id(8 + slot, slots.items[slot]))
        return false;
    }
    auto selected = [&](std::size_t index, std::uint8_t& out) {
      double value = -1;
      if (cells[index].tag() == sdk::Tag::Double)
        value = cells[index].as_double();
      else if (cells[index].tag() == sdk::Tag::Int)
        value = double(cells[index].as_int());
      if (!(value >= 0 && value < double(integration::kQuickslots)) || std::floor(value) != value)
        return false;
      out = std::uint8_t(value);
      return true;
    };
    return selected(12, slots.active_weapon) && selected(13, slots.active_item);
  }
  static Value quickslots_value(const integration::InventoryQuickslots& slots) {
    Value::Array list;
    for (std::size_t slot = 0; slot < integration::kQuickslots; ++slot) {
      list.push_back(Value(slots.primary[slot]));
      list.push_back(Value(slots.secondary[slot]));
    }
    for (const std::string& id : slots.items)
      list.push_back(Value(id));
    list.push_back(Value(double(slots.active_weapon)));
    list.push_back(Value(double(slots.active_item)));
    return Value(std::move(list));
  }
  // The stock of the resource host's inventoryShow message: { revision, rows: [[id, item (GUID
  // text), amount, quality, health, equipped]], quickslots (optional, quickslots_of) }. False for
  // anything malformed.
  static bool inventory_show_of(const Value& e, integration::InventoryShow& show) {
    const double revision = number(e, "revision", -1);
    const auto* rows = field(e, "rows");
    if (!(revision >= 0) || revision > 9e15 || std::floor(revision) != revision || !rows ||
        rows->tag() != sdk::Tag::Array || rows->as_array().size() > integration::kMaxInventoryRows)
      return false;
    show.revision = std::uint64_t(revision);
    if (const auto* replace = field(e, "replace"); replace && replace->tag() == sdk::Tag::Bool)
      show.replace = replace->as_bool();
    if (const auto* quickslots = field(e, "quickslots")) {
      show.quickslots.emplace();
      if (!quickslots_of(*quickslots, *show.quickslots))
        return false;
    }
    for (const Value& entry : rows->as_array()) {
      if (entry.tag() != sdk::Tag::Array || entry.as_array().size() != 6)
        return false;
      const auto& cells = entry.as_array();
      auto cell_number = [&](std::size_t index, double& out) {
        if (cells[index].tag() == sdk::Tag::Double)
          out = cells[index].as_double();
        else if (cells[index].tag() == sdk::Tag::Int)
          out = double(cells[index].as_int());
        else
          return false;
        return std::isfinite(out);
      };
      integration::InventoryRowState row;
      double amount = 0, quality = 0, health = 0, equipped = 0;
      if (cells[0].tag() != sdk::Tag::Str || cells[1].tag() != sdk::Tag::Str ||
          !cell_number(2, amount) || !cell_number(3, quality) || !cell_number(4, health) ||
          !cell_number(5, equipped) || !exact_uint32(amount) || !exact_uint32(equipped) ||
          !(quality >= 1 && quality <= 255) || !(health >= 0 && health <= 1))
        return false;
      row.id = cells[0].as_str();
      if (row.id.empty() || row.id.size() > integration::kMaxRowIdBytes ||
          !integration::parse_guid(cells[1].as_str(), row.item))
        return false;
      row.amount = std::uint32_t(amount);
      row.quality = std::uint8_t(quality);
      row.health = float(health);
      row.equipped = std::uint32_t(equipped);
      show.rows.push_back(std::move(row));
    }
    return true;
  }
  // A vendor's shelf as the resource host sends it (vendorShow).
  static bool vendor_show_of(const Value& e, integration::VendorShow& show) {
    integration::InventoryShow stock;
    const double session = number(e, "session", 0);
    const double npc = number(e, "npc", 0);
    const auto* prices = field(e, "prices");
    const auto* buy_prices = field(e, "buyPrices");
    const auto* purse = field(e, "purse");
    if (!exact_uint32(session) || session == 0 || !exact_uint32(npc) ||
        !inventory_show_of(e, stock) || stock.rows.size() > integration::kMaxVendorRows ||
        !prices || prices->tag() != sdk::Tag::Array ||
        prices->as_array().size() != stock.rows.size() || !buy_prices ||
        buy_prices->tag() != sdk::Tag::Array ||
        buy_prices->as_array().size() > integration::kMaxVendorBuyRows ||
        !integration::parse_guid(text(e, "money"), show.money)) {
      return false;
    }
    const auto price_of = [](const Value& cell, std::uint32_t& price) {
      double value = -1;
      if (cell.tag() == sdk::Tag::Double) {
        value = cell.as_double();
      } else if (cell.tag() == sdk::Tag::Int) {
        value = double(cell.as_int());
      }
      if (!exact_uint32(value) || value > integration::kMaxVendorPrice) {
        return false;
      }
      price = std::uint32_t(value);
      return true;
    };
    show.session = std::uint32_t(session);
    show.npc = std::uint32_t(npc);
    show.open = flag_of(e, "open");
    show.buys = flag_of(e, "buys");
    show.keeper_player = flag_of(e, "keeperPlayer");
    show.haggles = flag_of(e, "haggles");
    show.revision = stock.revision;
    show.purse_limited = purse && purse->tag() != sdk::Tag::Null;
    if (show.purse_limited && !price_of(*purse, show.purse)) {
      return false;
    }
    for (std::size_t index = 0; index < stock.rows.size(); ++index) {
      integration::VendorShelfRow shelf_row;
      shelf_row.row = std::move(stock.rows[index]);
      if (!price_of(prices->as_array()[index], shelf_row.price)) {
        return false;
      }
      show.shelf.push_back(std::move(shelf_row));
    }
    for (const Value& entry : buy_prices->as_array()) {
      if (entry.tag() != sdk::Tag::Array || entry.as_array().size() != 2 ||
          entry.as_array()[0].tag() != sdk::Tag::Str) {
        return false;
      }
      integration::VendorBuyPrice buy;
      if (!integration::parse_guid(entry.as_array()[0].as_str(), buy.item) ||
          !price_of(entry.as_array()[1], buy.price)) {
        return false;
      }
      show.buy_prices.push_back(buy);
    }
    return true;
  }
  // A door a client reported, as the resource host reads it.
  static Value door_entry_value(const integration::DoorEntry& door) {
    return Value(Value::Map{{"guid", Value(integration::door_guid_text(door.state.guid))},
                            {"name", Value(door.name)},
                            {"position", Value(Value::Map{{"x", Value(double(door.position[0]))},
                                                          {"y", Value(double(door.position[1]))},
                                                          {"z", Value(double(door.position[2]))}})},
                            {"open", Value(door.state.open)},
                            {"side", Value(door.state.side)},
                            {"locked", Value(door.state.locked)},
                            {"canBePicked", Value(door.canBePicked)},
                            {"key", Value(door.key)},
                            {"builtLocked", Value(door.builtLocked)},
                            {"difficulty", Value(double(door.difficulty))},
                            {"model", Value(door.model)}});
  }
  static bool flag_of(const Value& e, const char* key) {
    const sdk::Value* value = field(e, key);
    return value && value->tag() == sdk::Tag::Bool && value->as_bool();
  }
  // The numbers x, y, z (and w for a rotation) of a map field as floats; false when it is missing
  // or has a number that is not.
  template <std::size_t N>
  static bool floats_of(const Value& e, const char* key, std::array<float, N>& out) {
    const sdk::Value* value = field(e, key);
    if (!value || value->tag() != sdk::Tag::Map) {
      return false;
    }
    static constexpr const char* kNames[] = {"x", "y", "z", "w"};
    for (std::size_t i = 0; i < N; ++i) {
      out[i] = float(number(*value, kNames[i], std::nan("")));
    }
    return true;
  }
  // The ids of a marker or blip op: the player (`id`) and the object (`key`), both whole numbers.
  static bool marker_ids_of(const Value& e, const char* key, core::EntityId& player,
                            std::uint32_t& object) {
    const double player_id = number(e, "id", -1);
    const double object_id = number(e, key, -1);
    if (!exact_uint32(player_id) || !exact_uint32(object_id))
      return false;
    player = core::EntityId(player_id);
    object = std::uint32_t(object_id);
    return true;
  }
  // markerShow { id, marker, material, shape, position, rotation, size, height, spin, bob, color },
  // markerHide { id, marker }, blipShow { id, blip, type, markState, label, compass, map,
  // position }, blipHide { id, blip } (docs/ops/0.2.1-client-ops.md). False for a malformed op or
  // a player without a ready world.
  bool send_marker_op(const std::string& op, const Value& e) {
    core::EntityId player = core::kInvalidEntity;
    if (op == "markerHide") {
      std::uint32_t marker = 0;
      return marker_ids_of(e, "marker", player, marker) && server_.send_marker_hide(player, marker);
    }
    if (op == "blipHide") {
      std::uint32_t blip = 0;
      return marker_ids_of(e, "blip", player, blip) && server_.send_blip_hide(player, blip);
    }
    if (op == "markerShow") {
      integration::MarkerShow marker;
      std::array<float, 3> position{};
      std::array<float, 4> rotation{};
      if (!marker_ids_of(e, "marker", player, marker.marker) ||
          !floats_of(e, "position", position) || !floats_of(e, "rotation", rotation)) {
        return false;
      }
      std::copy(position.begin(), position.end(), marker.position);
      std::copy(rotation.begin(), rotation.end(), marker.rotation);
      marker.material = text(e, "material");
      marker.shape = text(e, "shape");
      marker.size = float(number(e, "size", 1));
      marker.height = float(number(e, "height", 0));
      marker.spin = float(number(e, "spin", 0));
      marker.bob = float(number(e, "bob", 0));
      const double color = number_or_null(e, "color", 0);
      if (!exact_uint32(color))
        return false;
      marker.color = std::uint32_t(color);
      return server_.send_marker_show(player, marker);
    }
    integration::BlipShow blip;
    std::array<float, 3> position{};
    const double state = number(e, "markState", 0);
    if (!marker_ids_of(e, "blip", player, blip.blip) || !floats_of(e, "position", position) ||
        !exact_uint32(state)) {
      return false;
    }
    std::copy(position.begin(), position.end(), blip.position);
    blip.type = text(e, "type");
    blip.markState = std::uint32_t(state);
    blip.label = text(e, "label");
    blip.compass = flag(e, "compass", true);
    blip.map = flag(e, "map", true);
    // KCDS: { image: { resource, path } }, the blip's own icon.
    if (const auto* image = field(e, "image"); image && image->tag() == sdk::Tag::Map) {
      blip.image_resource = text(*image, "resource");
      blip.image_path = text(*image, "path");
    }
    return server_.send_blip_show(player, blip);
  }
  // A string field, or false when it is missing or not a string.
  static bool string_of(const Value& v, const char* key, std::string& out) {
    const auto* x = field(v, key);
    if (!x || x->tag() != sdk::Tag::Str) {
      return false;
    }
    out = x->as_str();
    return true;
  }
  // One objective of a questShow: { text, progress, optional, position: { x, y, z } | null }.
  static bool quest_objective_of(const Value& v, integration::QuestObjective& objective) {
    std::string progress;
    if (v.tag() != sdk::Tag::Map || !string_of(v, "text", objective.text) ||
        !string_of(v, "progress", progress) ||
        !integration::quest_progress_of(progress, objective.progress)) {
      return false;
    }
    objective.optional = flag(v, "optional", false);
    const auto* position = field(v, "position");
    objective.has_position = position && position->tag() == sdk::Tag::Map;
    if (objective.has_position) {
      std::array<float, 3> values{};
      if (!floats_of(v, "position", values)) {
        return false;
      }
      std::copy(values.begin(), values.end(), objective.position);
    }
    return true;
  }
  // questShow { id, quest, key, title, description, type, progress, announce, objectives },
  // questHide { id, quest }, questTrack { id, quest, tracked }. False
  // for a malformed op or a player without a ready world.
  bool send_quest_op(const std::string& op, const Value& e) {
    core::EntityId player = core::kInvalidEntity;
    std::uint32_t quest = 0;
    if (!marker_ids_of(e, "quest", player, quest)) {
      return false;
    }
    if (op == "questHide") {
      return server_.send_quest_hide(player, quest);
    }
    if (op == "questTrack") {
      integration::QuestTracking track;
      track.quest = quest;
      track.tracked = flag(e, "tracked", false);
      return server_.send_quest_track(player, track);
    }
    integration::QuestShow show;
    show.quest = quest;
    std::string type;
    std::string progress;
    if (!string_of(e, "key", show.key) || !string_of(e, "title", show.title) ||
        !string_of(e, "description", show.description) || !string_of(e, "type", type) ||
        !integration::quest_type_of(type, show.type) || !string_of(e, "progress", progress) ||
        !integration::quest_progress_of(progress, show.progress)) {
      return false;
    }
    show.announce = flag(e, "announce", false);
    const auto* objectives = field(e, "objectives");
    if (!objectives || objectives->tag() != sdk::Tag::Array ||
        objectives->as_array().size() > integration::kQuestObjectivesMax) {
      return false;
    }
    for (const Value& entry : objectives->as_array()) {
      integration::QuestObjective objective;
      if (!quest_objective_of(entry, objective)) {
        return false;
      }
      show.objectives.push_back(std::move(objective));
    }
    return server_.send_quest_show(player, show);
  }
  // A number field that may also be null or missing (then `fallback`).
  static double number_or_null(const Value& v, const char* key, double fallback) {
    const auto* x = field(v, key);
    if (!x || (x->tag() != sdk::Tag::Double && x->tag() != sdk::Tag::Int))
      return fallback;
    return number(v, key, fallback);
  }
  // A boolean field (`fallback` when missing or not a boolean).
  static bool flag(const Value& v, const char* key, bool fallback) {
    const auto* x = field(v, key);
    return x && x->tag() == sdk::Tag::Bool ? x->as_bool() : fallback;
  }
  // A levelEditApply message as the wire's edit and its dimension. False for
  // anything malformed; a null position, rotation or scale is one the edit does not set.
  static bool level_edit_of(const Value& e, integration::LevelEdit& edit, std::uint32_t& world) {
    const double raw_id = number(e, "id");
    const double raw_world = number(e, "dimension", double(replication::kGlobalWorld));
    if (!exact_uint32(raw_id) || raw_id < 1 || !exact_uint32(raw_world)) {
      return false;
    }
    edit.id = std::uint32_t(raw_id);
    world = std::uint32_t(raw_world);
    const std::string kind = text(e, "kind");
    edit.name = text(e, "name");
    if (kind == "brush") {
      edit.kind = integration::LevelEditKind::Brush;
      edit.guid = 0;
    } else if (kind == "entity") {
      edit.kind = integration::LevelEditKind::Entity;
      if (!integration::parse_level_edit_guid(text(e, "guid"), edit.guid)) {
        return false;
      }
    } else if (kind == "layer") {
      edit.kind = integration::LevelEditKind::Layer;
      edit.guid = 0;
    } else {
      return false;
    }
    std::array<float, 3> at{};
    if (!floats_of(e, "at", at)) {
      return false;
    }
    std::copy(at.begin(), at.end(), edit.at);
    edit.hidden = flag_of(e, "hidden");
    std::array<float, 3> position{};
    std::array<float, 4> rotation{};
    std::array<float, 3> scale{};
    edit.has_position = floats_of(e, "position", position);
    edit.has_rotation = floats_of(e, "rotation", rotation);
    edit.has_scale = floats_of(e, "scale", scale);
    std::copy(position.begin(), position.end(), edit.position);
    if (edit.has_rotation) {
      std::copy(rotation.begin(), rotation.end(), edit.rotation);
    }
    if (edit.has_scale) {
      std::copy(scale.begin(), scale.end(), edit.scale);
    }
    return !integration::encode_level_edit(edit).empty();
  }
  // A siege engine's state as siege/engines.cjs stateOf writes it: { cycle, seconds,
  // speed, rotation, aimGuide, target, wrecked, usable, operator }. False for an unknown cycle or
  // a missing rotation or target.
  static bool siege_state_of(const Value& e, integration::SiegeEngineState& state) {
    static constexpr const char* kCycles[] = {"idle", "drawing", "loading", "ready", "firing"};
    const std::string cycle = text(e, "cycle");
    bool known = false;
    for (std::uint8_t i = 0; i < integration::kSiegeCycles; ++i) {
      if (cycle == kCycles[i]) {
        state.cycle = integration::SiegeCycle(i);
        known = true;
      }
    }
    state.seconds = float(number(e, "seconds", 0));
    state.speed = float(number(e, "speed", 1));
    state.aim_guide = flag_of(e, "aimGuide");
    state.wrecked = flag_of(e, "wrecked");
    state.usable = flag_of(e, "usable");
    state.operated = number(e, "operator", 0) != 0;
    return known && floats_of(e, "rotation", state.rotation) &&
           floats_of(e, "target", state.target);
  }
  // A siege ladder's pose as siege/ladders.cjs names it; false for another name.
  static bool ladder_pose_of(const Value& e, integration::LadderPose& pose) {
    static constexpr const char* kPoses[] = {"standing", "pushed", "down", "raising", "broken"};
    const std::string name = text(e, "pose");
    for (std::uint8_t i = 0; i < integration::kLadderPoses; ++i) {
      if (name == kPoses[i]) {
        pose = integration::LadderPose(i);
        return true;
      }
    }
    return false;
  }
  // A siege ladder's kind as siege/ladders.cjs KINDS names it; false for another name.
  static bool ladder_kind_of(const Value& e, integration::LadderKind& kind) {
    static constexpr const char* kKinds[] = {"siege", "tall", "short", "low"};
    const std::string name = text(e, "kind");
    for (std::uint8_t i = 0; i < integration::kLadderKinds; ++i) {
      if (name == kKinds[i]) {
        kind = integration::LadderKind(i);
        return true;
      }
    }
    return false;
  }
  // A stone pile's stones as siege/stones.cjs holds them: -1 never runs out, else a whole count.
  static bool pile_stones_of(const Value& e, std::int64_t& stones) {
    const double value = number(e, "stones", -1);
    if (value < 0) {
      stones = -1;
      return true;
    }
    if (!exact_uint32(value) || value > double(integration::kMaxPileStones)) {
      return false;
    }
    stones = std::int64_t(value);
    return true;
  }
  // The siege op's entity id: a whole number 1 .. 2^32 - 1, else 0.
  static std::uint32_t siege_id_of(const Value& e, const char* key) {
    const double id = number(e, key, 0);
    if (!(id >= 1) || id > 4294967295.0 || id != std::floor(id))
      return 0;
    return std::uint32_t(id);
  }
  // An order of the resource host's animation message: { kind: play | stop, request (a whole
  // number), then for play: fragment, clip, tags, loop, lockMovement, restore, props ([{ item
  // (GUID text) or model, hand: right | left, joint, position {x, y, z}, rotation {x, y, z, w}
  // }]), for stop: standUp }. False for anything malformed.
  static bool animation_order_of(const Value& e, integration::AnimationOrder& order) {
    const double request = number(e, "request", -1);
    if (!(request >= 0) || request > double(integration::kAnimationRequestMax) ||
        request != std::floor(request)) {
      return false;
    }
    order.request = std::uint64_t(request);
    const std::string kind = text(e, "kind");
    if (kind == "stop") {
      order.kind = integration::AnimationOrder::Kind::Stop;
      order.stand_up = flag_of(e, "standUp");
      return true;
    }
    if (kind == "face") {
      order.kind = integration::AnimationOrder::Kind::Face;
      order.fragment = text(e, "fragment");
      order.tags = text(e, "tags");
      return true;
    }
    if (kind == "talk") {
      order.kind = integration::AnimationOrder::Kind::Talk;
      order.fragment = text(e, "fragment");
      return true;
    }
    if (kind == "speed") {
      order.kind = integration::AnimationOrder::Kind::Speed;
      order.speed = float(number(e, "speed", -1));
      return true;
    }
    if (kind == "props") {
      order.kind = integration::AnimationOrder::Kind::Props;
    } else if (kind != "play") {
      return false;
    } else {
      order.kind = integration::AnimationOrder::Kind::Play;
    }
    if (order.kind == integration::AnimationOrder::Kind::Play) {
      order.speed = float(number(e, "speed", 1));
      order.start = float(number(e, "time", 0));
    }
    order.fragment = text(e, "fragment");
    order.clip = text(e, "clip");
    order.tags = text(e, "tags");
    order.loop = flag_of(e, "loop");
    order.lock_movement = flag_of(e, "lockMovement");
    order.upper_body = flag_of(e, "upperBody");
    order.no_gravity = flag_of(e, "noGravity");
    order.lock_rotation = flag_of(e, "lockRotation");
    // lockRotation's limits (degrees, 0 the default): lookYaw 0 .. 180, lookUp and lookDown
    // 0 .. 89.
    const auto limit = [&](const char* key, double highest) {
      const double value = number(e, key, 0);
      return std::uint8_t(std::clamp(std::floor(std::isfinite(value) ? value : 0), 0.0, highest));
    };
    order.look_yaw = limit("lookYaw", 180);
    order.look_up = limit("lookUp", 89);
    order.look_down = limit("lookDown", 89);
    const double partner = number(e, "partner", 0);
    if (partner < 0 || partner > 4294967295.0 || partner != std::floor(partner)) {
      return false;
    }
    order.partner = std::uint32_t(partner);
    order.paired_slave = flag_of(e, "pairedSlave");
    order.restore = flag_of(e, "restore");
    const sdk::Value* props = field(e, "props");
    if (!props) {
      return true;
    }
    if (props->tag() != sdk::Tag::Array ||
        props->as_array().size() > integration::kAnimationPropsMax) {
      return false;
    }
    for (const Value& entry : props->as_array()) {
      integration::AnimationProp prop;
      const std::string item = text(entry, "item");
      if (!item.empty()) {
        integration::Guid guid{};
        if (!integration::parse_guid(item, guid)) {
          return false;
        }
        prop.item = guid;
      }
      prop.model = text(entry, "model");
      prop.left_hand = text(entry, "hand") == "left";
      prop.joint = text(entry, "joint");
      prop.from_animation = flag_of(entry, "fromAnimation");
      prop.until_put_down = flag_of(entry, "untilPutDown");
      if (prop.from_animation && prop.until_put_down) {
        return false;
      }
      // The server Prop it rests as before and after the animation.
      const double rest = number(entry, "rest", 0);
      if (rest < 0 || rest > 4294967295.0 || (rest > 0 && !prop.from_animation)) {
        return false;
      }
      prop.rest = std::uint32_t(rest);
      if (field(entry, "position")) {
        std::array<float, 3> position{};
        if (!floats_of(entry, "position", position)) {
          return false;
        }
        prop.position = position;
      }
      if (field(entry, "rotation")) {
        std::array<float, 4> rotation{};
        if (!floats_of(entry, "rotation", rotation)) {
          return false;
        }
        prop.rotation = rotation;
      }
      order.props.push_back(std::move(prop));
    }
    return true;
  }
  // An order of the resource host's buffOrder message: { kind: add | remove | clear | claim,
  // buff (GUID text), instances ([decimal text]: 64-bit ids), tag (number), classes ([number]) }.
  // False for anything malformed.
  static bool buff_order_of(const Value& e, integration::BuffOrder& order) {
    using Kind = integration::BuffOrder::Kind;
    const std::string kind = text(e, "kind");
    if (kind == "add") {
      order.kind = Kind::Add;
      return integration::parse_guid(text(e, "buff"), order.buff);
    }
    if (kind == "remove") {
      order.kind = Kind::Remove;
      const auto* instances = field(e, "instances");
      if (!integration::parse_guid(text(e, "buff"), order.buff) || !instances ||
          instances->tag() != sdk::Tag::Array ||
          instances->as_array().size() > integration::kMaxBuffs)
        return false;
      for (const Value& item : instances->as_array()) {
        std::uint64_t instance = 0;
        if (item.tag() != sdk::Tag::Str || !decimal_u64(item.as_str(), instance))
          return false;
        order.instances.push_back(instance);
      }
      return true;
    }
    if (kind == "clear") {
      order.kind = Kind::Clear;
      const auto tag = number(e, "tag", -1);
      if (tag < 0 || tag > 255)
        return false;
      order.tag = std::uint8_t(tag);
      return true;
    }
    if (kind == "claim") {
      order.kind = Kind::Claim;
      const auto* classes = field(e, "classes");
      if (!classes || classes->tag() != sdk::Tag::Array ||
          classes->as_array().size() > integration::kMaxBuffClasses)
        return false;
      for (const Value& item : classes->as_array()) {
        // The host's numbers arrive as doubles.
        double id = -1;
        if (item.tag() == sdk::Tag::Int)
          id = double(item.as_int());
        else if (item.tag() == sdk::Tag::Double)
          id = item.as_double();
        if (!(id >= 0) || id >= integration::kMaxBuffClasses || id != std::floor(id))
          return false;
        order.classes.push_back(std::uint8_t(id));
      }
      return true;
    }
    return false;
  }
  // A show or hide of the host's customBuff message: { kind: show | hide, buff
  // (name), iconId (GUID text), label, duration and remaining (seconds, -1 none) }. False for
  // anything malformed.
  static bool custom_buff_of(const Value& e, integration::CustomBuff& buff) {
    using Kind = integration::CustomBuff::Kind;
    const std::string kind = text(e, "kind");
    buff.name = text(e, "buff");
    if (kind == "hide") {
      buff.kind = Kind::Hide;
      return integration::custom_buff_name_valid(buff.name);
    }
    if (kind != "show") {
      return false;
    }
    buff.kind = Kind::Show;
    buff.label = text(e, "label");
    if (!integration::parse_guid(text(e, "iconId"), buff.icon)) {
      return false;
    }
    buff.duration_ms = custom_buff_ms(number(e, "duration", -1));
    buff.remaining_ms = custom_buff_ms(number(e, "remaining", -1));
    buff.params = text(e, "params");
    // { image: { resource, path } }, the buff's own picture.
    if (const auto* image = field(e, "image"); image && image->tag() == sdk::Tag::Map) {
      buff.image_resource = text(*image, "resource");
      buff.image_path = text(*image, "path");
    }
    return integration::custom_buff_name_valid(buff.name) &&
           integration::custom_buff_label_valid(buff.label) &&
           integration::custom_buff_time_valid(buff.duration_ms) &&
           integration::custom_buff_time_valid(buff.remaining_ms) &&
           integration::custom_buff_params_valid(buff.params) &&
           integration::custom_buff_image_valid(buff.image_resource, buff.image_path);
  }
  // Seconds as whole milliseconds; anything below zero (or not a number) is -1, without end.
  static std::int64_t custom_buff_ms(double seconds) {
    if (!(seconds >= 0)) {
      return -1;
    }
    if (seconds > double(integration::kCustomBuffDurationMaxMs) / 1000.0) {
      return integration::kCustomBuffDurationMaxMs + 1;
    }
    return std::int64_t(std::llround(seconds * 1000.0));
  }
  // A 64-bit unsigned number from its decimal text; false for anything else.
  static bool decimal_u64(const std::string& text, std::uint64_t& value) {
    if (text.empty() || text.size() > 20)
      return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size();
  }
  static bool snapshot_of(const Value& v, integration::Progression& snapshot) {
    const auto* tracks = field(v, "tracks");
    const auto* perks = field(v, "perks");
    if (!tracks || tracks->tag() != sdk::Tag::Array || !perks || perks->tag() != sdk::Tag::Array ||
        tracks->as_array().size() != integration::kTracks.size() ||
        perks->as_array().size() > integration::kMaxPerks)
      return false;
    for (std::size_t i = 0; i < integration::kTracks.size(); ++i) {
      const Value& entry = tracks->as_array()[i];
      if (entry.tag() != sdk::Tag::Array || entry.as_array().size() != 3)
        return false;
      const auto value = [&](std::size_t at) {
        const Value& x = entry.as_array()[at];
        if (x.tag() == sdk::Tag::Int)
          return double(x.as_int());
        return x.tag() == sdk::Tag::Double ? x.as_double() : -1.0;
      };
      const double level = value(0);
      const double xp = value(1) * integration::kXpUnits;
      const double points = value(2);
      if (level < 0 || level > integration::kLevelCap || !(xp >= 0) ||
          xp > double(integration::kMaxOrderXp) || points < 0 || points > 0xFFFF)
        return false;
      snapshot.tracks[i] = {std::uint8_t(level), std::uint32_t(std::llround(xp)),
                            std::uint16_t(points)};
    }
    const double main_points = number(v, "mainPoints", 0);
    if (main_points < 0 || main_points > 0xFFFF)
      return false;
    snapshot.main_points = std::uint16_t(main_points);
    for (const Value& perk : perks->as_array()) {
      integration::PerkId id{};
      if (perk.tag() != sdk::Tag::Str || !integration::parse_perk_id(perk.as_str(), id))
        return false;
      snapshot.perks.push_back(id);
    }
    integration::normalize_perks(snapshot.perks);
    return true;
  }
  // The name the player connected under, or "Player <id>" while it has none.
  std::string nickname_of(core::EntityId id) const {
    if (auto name = server_.world().read(id, Nickname)) {
      if (!std::get<std::string>(*name).empty())
        return std::get<std::string>(*name);
    }
    return "Player " + std::to_string(id);
  }
  // Player.equipment: the item classes the owner reports wearing, as 32 hex digits.
  // The "outfit" field is "KO2" and per piece its class's 16 bytes, its quality and health (3
  // bytes); "KO1" has the class bytes only (Client/runtime/appearance/outfit.cpp).
  static Value equipment_value(const std::string& outfit) {
    constexpr std::string_view kTag = "KO2";
    constexpr std::string_view kClassesOnlyTag = "KO1";
    Value::Array classes;
    const bool classes_only = outfit.size() >= kClassesOnlyTag.size() &&
                              outfit.compare(0, kClassesOnlyTag.size(), kClassesOnlyTag) == 0;
    const std::size_t piece_bytes = classes_only ? 16 : 16 + 3;
    if (outfit.size() < kTag.size() ||
        (!classes_only && outfit.compare(0, kTag.size(), kTag) != 0) ||
        (outfit.size() - kTag.size()) % piece_bytes != 0)
      return Value(std::move(classes));
    for (std::size_t at = kTag.size(); at < outfit.size(); at += piece_bytes) {
      integration::ItemClassId id{};
      std::memcpy(id.data(), outfit.data() + at, id.size());
      classes.push_back(Value(integration::item_class_script_text(id)));
    }
    return Value(std::move(classes));
  }
  // Player.rightHandItem and leftHandItem: [right, left] as 32 hex digits, "" for an
  // empty hand.
  static Value hands_value(const std::string& field) {
    integration::Hands hands;
    if (!integration::parse_hands_field(field, hands)) {
      hands = {};
    }
    auto text = [](const std::optional<integration::ItemClassId>& id) {
      return Value(id ? integration::item_class_script_text(*id) : std::string());
    };
    return Value(Value::Array{text(hands.right), text(hands.left)});
  }
  // Player.grime: { body: { dirt, blood: [6] }, items: [{ item, dirt, blood: [6],
  // wear }] } with values 0 .. 1; null before the owner reported it.
  static Value grime_value(const std::string& field) {
    integration::Grime grime;
    if (!integration::parse_grime_field(field, grime)) {
      return Value();
    }
    auto fraction = [](std::uint8_t byte) { return Value(double(byte) / 255.0); };
    auto state = [&](const integration::GrimeState& s, Value::Map map) {
      Value::Array blood;
      for (const std::uint8_t zone : s.blood) {
        blood.push_back(fraction(zone));
      }
      map.emplace_back("dirt", fraction(s.dirt));
      map.emplace_back("blood", Value(std::move(blood)));
      return Value(std::move(map));
    };
    Value::Array items;
    for (const auto& piece : grime.pieces) {
      items.push_back(state(
          piece.state, Value::Map{{"item", Value(integration::item_class_script_text(piece.item))},
                                  {"wear", fraction(piece.wear)}}));
    }
    return Value(
        Value::Map{{"body", state(grime.body, Value::Map{})}, {"items", Value(std::move(items))}});
  }
  // The class a player carries (GUID text), "" for none or a field that is not one.
  static Value carry_value(const std::string& field) {
    integration::Guid item{};
    if (!integration::parse_carry_field(field, item)) {
      return Value(std::string());
    }
    return Value(integration::guid_text(item));
  }
  static Value appearance_value(const integration::Appearance& appearance) {
    return Value(Value::Map{{"gender", Value(appearance.gender)},
                            {"body", Value(appearance.body)},
                            {"head", Value(appearance.head)},
                            {"hair", Value(appearance.hair)},
                            {"beard", Value(appearance.beard)}});
  }
  // Sends a resource horse's state when it changed since the last report.
  void report_horse(std::uint32_t script, core::EntityId id) {
    auto pos = server_.world().read(id, 0);
    auto rot = server_.world().read(id, 1);
    auto rider = server_.world().read(id, 7);
    const auto* slot = server_.world().slot(id);
    if (!pos || !rot || !rider || !slot)
      return;
    HorseReport report;
    report.dimension = slot->dimension;
    report.position = std::get<core::Vec3>(*pos);
    report.rotation = std::get<core::Quat>(*rot);
    report.rider = std::get<std::int64_t>(*rider);
    if (!horse_reports_.changed(script, report))
      return;
    scripts_->send(Value(Value::Map{{"op", Value("horseState")},
                                    {"id", Value(std::int64_t(script))},
                                    {"position", vector_value(report.position)},
                                    {"rotation", rotation_value(report.rotation)},
                                    {"dimension", Value(std::int64_t(report.dimension))},
                                    {"riderId", Value(report.rider)}}));
  }
  // Sends a resource NPC's state when it changed since the last report (its simulator moves it).
  void report_npc(std::uint32_t script, core::EntityId id) {
    const auto* slot = server_.world().slot(id);
    const auto* position = server_.world().read(id, integration::NpcPosition);
    const auto* rotation = server_.world().read(id, integration::NpcRotation);
    if (!slot || !position || !rotation) {
      return;
    }
    NpcReport report;
    report.dimension = slot->dimension;
    report.position = std::get<core::Vec3>(*position);
    report.rotation = std::get<core::Quat>(*rotation);
    report.velocity = vector_of(id, integration::NpcVelocity);
    if (!npc_reports_.changed(script, report)) {
      return;
    }
    scripts_->send(Value(Value::Map{{"op", Value("npcState")},
                                    {"id", Value(std::int64_t(script))},
                                    {"position", vector_value(report.position)},
                                    {"rotation", rotation_value(report.rotation)},
                                    {"velocity", vector_value(report.velocity)},
                                    {"dimension", Value(std::int64_t(report.dimension))}}));
  }
  // The NPC ops of the resource host (Npc): npcSpawn, npcDestroy, npcName,
  // npcFrozen, npcWorld and npcTeleport. The host checked the arguments; the server checks them
  // again and a refusal is logged.
  void npc_op(const std::string& op, const Value& e) {
    const double raw_id = number(e, "id");
    if (!exact_uint32(raw_id)) {
      std::cerr << "RESOURCE npc refused: malformed id in " << op << std::endl;
      return;
    }
    const auto script_id = std::uint32_t(raw_id);
    if (op == "npcSpawn") {
      spawn_resource_npc(script_id, e);
      return;
    }
    auto found = npcs_.find(script_id);
    if (found == npcs_.end()) {
      std::cerr << "RESOURCE npc refused: unknown " << script_id << " in " << op << std::endl;
      return;
    }
    const core::EntityId id = found->second;
    // Report the NPC's authoritative state again, also after a refused request.
    npc_reports_.forget(script_id);
    bool done = false;
    if (op == "npcDestroy") {
      server_.despawn(id);
      npcs_.erase(found);
      std::cout << "NPC destroy script=" << script_id << std::endl;
      done = true;
    } else if (op == "npcName") {
      done = server_.set_npc_name(id, text(e, "name"));
    } else if (op == "npcFrozen") {
      const auto* frozen = field(e, "frozen");
      done = frozen && frozen->tag() == sdk::Tag::Bool &&
             server_.set_npc_frozen(id, frozen->as_bool());
    } else if (op == "npcDefense") {
      done = server_.set_npc_defense(id, float(number(e, "defense")));
    } else if (op == "npcDamage") {
      done = server_.set_npc_damage(id, float(number(e, "damage")));
    } else if (op == "npcWorld") {
      const double world = number(e, "dimension");
      done = exact_uint32(world) && server_.set_npc_dimension(id, std::uint32_t(world));
    } else if (op == "npcAnimation") {
      done = server_.set_npc_animation(id, text(e, "animation"));
    } else if (op == "npcIntent") {
      done = server_.set_npc_intent(id, text(e, "intent"));
    } else if (op == "npcSay") {
      done = server_.set_npc_speech(id, text(e, "speech"));
    } else if (op == "npcNameplate") {
      done = server_.set_npc_nameplate(id, text(e, "field"));
    } else if (op == "npcLocomotion") {
      const auto* kinematic = field(e, "kinematic");
      done = kinematic && kinematic->tag() == sdk::Tag::Bool &&
             server_.set_npc_kinematic(id, kinematic->as_bool());
    } else if (op == "npcBrain") {
      const auto* brain = field(e, "brain");
      done = brain && brain->tag() == sdk::Tag::Bool && server_.set_npc_brain(id, brain->as_bool());
    } else if (op == "npcAttackable") {
      const auto* attackable = field(e, "attackable");
      done = attackable && attackable->tag() == sdk::Tag::Bool &&
             server_.set_npc_attackable(id, attackable->as_bool());
    } else if (op == "npcLevelGuid") {
      const auto* guid = field(e, "guid");
      done = guid && guid->tag() == sdk::Tag::Str && server_.set_npc_level_guid(id, guid->as_str());
    } else if (op == "npcInteractable") {
      const auto* interactable = field(e, "interactable");
      done = interactable && interactable->tag() == sdk::Tag::Bool &&
             server_.set_npc_interactable(id, interactable->as_bool());
    } else if (op == "npcNameShown") {
      const auto* shown = field(e, "shown");
      done = shown && shown->tag() == sdk::Tag::Bool &&
             server_.set_npc_name_shown(id, shown->as_bool());
    } else if (op == "npcFace") {
      const auto* face = field(e, "face");
      done = face && face->tag() == sdk::Tag::Str && server_.set_npc_face(id, face->as_str());
    } else if (op == "npcTalking") {
      const auto* style = field(e, "style");
      done = style && style->tag() == sdk::Tag::Str && server_.set_npc_talking(id, style->as_str());
    } else if (op == "npcTargetable") {
      const auto* targetable = field(e, "targetable");
      done = targetable && targetable->tag() == sdk::Tag::Bool &&
             server_.set_npc_targetable(id, targetable->as_bool());
    } else if (op == "npcCarcassClosed") {
      const auto* closed = field(e, "closed");
      done = closed && closed->tag() == sdk::Tag::Bool &&
             server_.set_npc_carcass_closed(id, closed->as_bool());
    } else if (op == "npcPin") {
      const double player = number(e, "player");
      done = exact_uint32(player) && server_.pin_npc(id, core::EntityId(player));
    } else if (op == "npcHealth") {
      const auto* alive = field(e, "alive");
      done = alive && alive->tag() == sdk::Tag::Bool &&
             server_.set_npc_health(id, float(number(e, "health")), alive->as_bool());
    } else if (op == "npcAppearance") {
      done = server_.set_npc_appearance(id, text(e, "appearance"));
    } else if (op == "npcOutfit") {
      std::string outfit;
      done = outfit_of_pieces(e, outfit) && server_.set_npc_outfit(id, outfit);
    } else if (op == "npcTeleport") {
      const auto* position = field(e, "position");
      const auto* rotation = field(e, "rotation");
      if (position && rotation) {
        const core::Vec3 p{float(number(*position, "x")), float(number(*position, "y")),
                           float(number(*position, "z"))};
        const core::Quat q{float(number(*rotation, "x")), float(number(*rotation, "y")),
                           float(number(*rotation, "z")), float(number(*rotation, "w", 1))};
        done = server_.teleport_npc(id, p, q);
      }
    }
    if (!done) {
      std::cerr << "RESOURCE npc refused: " << op << " for " << script_id << std::endl;
    }
  }
  // { pieces: [{ itemClass (32 hex digits), quality, health 0 .. 1 }] } (Npc.setOutfit,
  // Horse.setTack) as the outfit field ("KO2", per piece the class bytes, the quality and the
  // health in 65535ths); false for a piece that is not one or more pieces than the field holds.
  bool outfit_of_pieces(const Value& e, std::string& outfit) {
    outfit = "KO2";
    const auto* pieces = field(e, "pieces");
    if (!pieces || pieces->tag() != sdk::Tag::Array) {
      return false;
    }
    for (const Value& piece : pieces->as_array()) {
      integration::ItemClassId item{};
      const double quality = number(piece, "quality", 1);
      const double health = number(piece, "health", 1);
      const bool valid =
          integration::parse_item_class_script_text(text(piece, "itemClass"), item) &&
          quality >= 1 && quality <= 255 && health >= 0 && health <= 1;
      if (!valid) {
        return false;
      }
      const auto steps = std::uint16_t(std::lround(health * 65535));
      outfit.append(reinterpret_cast<const char*>(item.data()), item.size());
      outfit.push_back(char(std::uint8_t(quality)));
      outfit.push_back(char(steps & 0xff));
      outfit.push_back(char(steps >> 8));
    }
    return integration::valid_npc_fixed_outfit(outfit);
  }
  // npcSpawn { id, position, rotation, soul, actorClass, name, dimension, frozen }: the resource
  // host named the NPC with an id the world reserved for it, so the same id is its network id in
  // scripts, on the wire and on every client.
  void spawn_resource_npc(std::uint32_t script_id, const Value& e) {
    const auto* position = field(e, "position");
    const auto* rotation = field(e, "rotation");
    const auto* frozen = field(e, "frozen");
    const double world = number(e, "dimension", double(replication::kGlobalWorld));
    if (npcs_.count(script_id) || !position || !rotation || !exact_uint32(world)) {
      std::cerr << "RESOURCE npc refused: malformed spawn " << script_id << std::endl;
      return;
    }
    integration::NpcSpawn spawn;
    spawn.position = core::Vec3{float(number(*position, "x")), float(number(*position, "y")),
                                float(number(*position, "z"))};
    spawn.rotation = core::Quat{float(number(*rotation, "x")), float(number(*rotation, "y")),
                                float(number(*rotation, "z")), float(number(*rotation, "w", 1))};
    spawn.soul = text(e, "soul");
    spawn.actor_class = text(e, "actorClass");
    spawn.name = text(e, "name");
    spawn.dimension = std::uint32_t(world);
    spawn.frozen = frozen && frozen->tag() == sdk::Tag::Bool && frozen->as_bool();
    const core::EntityId id = server_.spawn_npc(spawn, script_id);
    if (id == core::kInvalidEntity) {
      std::cerr << "RESOURCE npc refused: spawn " << script_id
                << " (invalid values or an id that was not reserved)" << std::endl;
      return;
    }
    npcs_[script_id] = id;
    std::cout << "NPC resource spawn script=" << script_id << " class=" << spawn.actor_class
              << " pos=" << spawn.position.x << "," << spawn.position.y << "," << spawn.position.z
              << std::endl;
  }
  // The changes of NPC simulators (npcSimulatorChange), after the views and players they name.
  void report_npc_simulators(const std::vector<integration::NpcSimulatorChange>& changes) {
    for (const auto& change : changes) {
      if (!resource_npc(change.npc)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("npcSimulator")},
                                      {"id", Value(std::int64_t(change.npc))},
                                      {"simulatorId", Value(std::int64_t(change.simulator))},
                                      {"previousId", Value(std::int64_t(change.previous))},
                                      {"epoch", Value(change.epoch)}}));
    }
  }
  // Health a client's game would have taken off an NPC (NPC_HIT): the resource host
  // decides what the NPC loses (npcDamage, npcDeath) and answers with npcHealth.
  void report_npc_hits(const std::vector<integration::NpcHit>& hits) {
    for (const auto& hit : hits) {
      if (!resource_npc(hit.npc)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("npcHit")},
                                      {"id", Value(std::int64_t(hit.npc))},
                                      {"attackerId", Value(std::int64_t(hit.attacker))},
                                      {"amount", Value(double(hit.amount))},
                                      {"reason", Value(std::int64_t(hit.reason))},
                                      {"subpart", Value(std::int64_t(hit.subpart))}}));
    }
  }
  // A player pressed the use key looking at a server NPC (NPC_USE): the resource host
  // checks the range and `interactable` and raises npcInteract.
  void report_npc_uses(const std::vector<integration::NpcUse>& uses) {
    for (const auto& use : uses) {
      if (!resource_npc(use.npc)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("npcInteract")},
                                      {"id", Value(std::int64_t(use.npc))},
                                      {"playerId", Value(std::int64_t(use.player))}}));
    }
  }
  // A player locked on to a server NPC (NPC_ENGAGE): the resource host lets an animal
  // that fights back attack them.
  void report_npc_engages(const std::vector<integration::NpcUse>& engages) {
    for (const auto& engage : engages) {
      if (!resource_npc(engage.npc)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("npcEngage")},
                                      {"id", Value(std::int64_t(engage.npc))},
                                      {"playerId", Value(std::int64_t(engage.player))}}));
    }
  }
  // The clip a resource played on an NPC is over (NPC_ANIMATION_END): the resource
  // host raises npcAnimationEnd for the request the sequence names.
  void report_npc_animation_ends(const std::vector<integration::NpcAnimationEnd>& ends) {
    for (const auto& end : ends) {
      if (!resource_npc(end.npc)) {
        continue;
      }
      std::string reason = "finished";
      if (end.reason == integration::kNpcAnimationInterrupted) {
        reason = "interrupted";
      } else if (end.reason == integration::kNpcAnimationRefused) {
        reason = "refused";
      }
      scripts_->send(Value(Value::Map{{"op", Value("npcAnimationEnd")},
                                      {"id", Value(std::int64_t(end.npc))},
                                      {"reason", Value(reason)},
                                      {"sequence", Value(std::int64_t(end.sequence))}}));
    }
  }
  // The order an NPC's simulator walks ended (NPC_INTENT_DONE): the resource host
  // raises npcIntentDone and steps patrols on. The sequence names the order it ended.
  void report_npc_intent_dones(const std::vector<integration::NpcIntentDone>& dones) {
    for (const auto& done : dones) {
      // A horse's order reports the same way: the host raises horseIntentDone.
      const bool horse = resource_horse(done.npc);
      if (!horse && !resource_npc(done.npc)) {
        continue;
      }
      std::string status = "failed";
      if (done.status == integration::kNpcIntentReached) {
        status = "reached";
      } else if (done.status == integration::kNpcIntentBlocked) {
        status = "blocked";
      }
      scripts_->send(Value(Value::Map{{"op", Value(horse ? "horseIntentDone" : "npcIntentDone")},
                                      {"id", Value(std::int64_t(done.npc))},
                                      {"status", Value(status)},
                                      {"sequence", Value(std::int64_t(done.sequence))}}));
    }
  }
  // A player changed dimension: the edits of the world left go back, those of the
  // world entered (and of the global world) are applied; the send itself picks which is which.
  void resend_level_edits(core::EntityId player) {
    for (const auto& [id, entry] : level_edits_) {
      if (!server_.send_level_edit(player, entry.first, entry.second)) {
        server_.send_level_edit_remove(player, id);
      }
    }
  }
  // What a resource horse's simulator reported of its health and state (HORSE_HEALTH),
  // after the server's rules: the resource host updates its ledger and raises horseDamage,
  // horseDeath, horseKnockedOut and the others.
  void report_horse_stamina_changes(const std::vector<integration::HorseStaminaChange>& changes) {
    for (const auto& change : changes) {
      if (!resource_horse(change.horse)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("horseStamina")},
                                      {"id", Value(std::int64_t(change.horse))},
                                      {"stamina", Value(double(change.stamina))}}));
    }
  }
  void report_horse_health_changes(const std::vector<integration::HorseHealthChange>& changes) {
    for (const auto& change : changes) {
      if (!resource_horse(change.horse)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("horseHealth")},
                                      {"id", Value(std::int64_t(change.horse))},
                                      {"health", Value(double(change.health))},
                                      {"state", Value(change.state)}}));
    }
  }
  // The resource host changed its local copy of an entity (a forced transform or world request).
  // Report the authoritative state again even if the native value did not change.
  void forget_reported_entity(core::EntityId id) {
    player_reports_.forget(id);
    for (const auto& [script, native] : horses_) {
      if (native == id)
        horse_reports_.forget(script);
    }
    if (resource_npc(id)) {
      npc_reports_.forget(std::uint32_t(id));
    }
  }
  // Public record of a visible entity, or nothing for an entity without a transform.
  std::optional<resources::ReportedEntity> entity_record(core::EntityId id) const {
    const auto* slot = server_.world().slot(id);
    auto position = server_.world().read(id, 0);
    auto rotation = server_.world().read(id, 1);
    if (!slot || !position || !rotation || !std::holds_alternative<core::Vec3>(*position) ||
        !std::holds_alternative<core::Quat>(*rotation))
      return std::nullopt;
    resources::ReportedEntity record;
    record.native_id = id;
    record.script_id = id;
    record.kind = "entity";
    record.dimension = slot->dimension;
    record.position = std::get<core::Vec3>(*position);
    record.rotation = std::get<core::Quat>(*rotation);
    if (slot->type == core::type_id("NetworkPlayer")) {
      record.kind = "player";
      if (auto appearance = server_.world().read(id, Appearance)) {
        integration::parse_appearance_field(std::get<std::string>(*appearance), record.appearance);
      }
      if (auto disguise = server_.world().read(id, Disguise)) {
        record.disguise = std::get<std::string>(*disguise);
      }
      if (auto visible = server_.world().read(id, BodyVisible)) {
        record.visible = std::get<bool>(*visible);
      }
      if (auto visor = server_.world().read(id, VisorClosed)) {
        record.visor_closed = std::get<bool>(*visor);
      }
      record.nickname = nickname_of(id);
      record.player_index = player_slots_.slot_of(id);
      record.ready = server_.player_ready(id);
      record.vitals = vitals_of(id);
      record.stats = stats_of(id);
      record.abilities = abilities_of(id);
      record.velocity = vector_of(id, Velocity);
      record.motion_velocity = vector_of(id, MotionVelocity);
      record.look_direction = vector_of(id, LookDirection);
      record.horse_id = horse_of(id);
      record.posture = posture_of(id);
      if (auto outfit = server_.world().read(id, Outfit)) {
        record.outfit = std::get<std::string>(*outfit);
      }
      if (auto hands = server_.world().read(id, Hands)) {
        record.hands = std::get<std::string>(*hands);
      }
    } else if (slot->type == core::type_id("NetworkHorse")) {
      auto public_id = server_.world().read(id, 4);
      if (!public_id || !std::holds_alternative<std::int64_t>(*public_id))
        return std::nullopt;
      record.script_id = std::uint32_t(std::get<std::int64_t>(*public_id));
      record.kind = "horse";
    }
    return record;
  }
  static Value entity_record_value(const resources::ReportedEntity& record) {
    Value::Map value{{"id", Value(std::int64_t(record.script_id))},
                     {"nativeId", Value(std::int64_t(record.native_id))},
                     {"kind", Value(record.kind)},
                     {"dimension", Value(std::int64_t(record.dimension))},
                     {"position", vector_value(record.position)},
                     {"rotation", rotation_value(record.rotation)}};
    if (record.kind == "player") {
      value.emplace_back("appearance", appearance_value(record.appearance));
      value.emplace_back("disguise", Value(record.disguise));
      value.emplace_back("visible", Value(record.visible));
      value.emplace_back("visorDown", Value(record.visor_closed));
      value.emplace_back("nickname", Value(record.nickname));
      value.emplace_back("playerIndex", Value(std::int64_t(record.player_index)));
      value.emplace_back("ready", Value(record.ready));
      value.emplace_back("vitals", vitals_value(record.vitals));
      value.emplace_back("stats", stats_value(record.stats));
      value.emplace_back("scaledStats", relative_stats_value(record.stats));
      value.emplace_back("abilities", abilities_value(record.abilities));
      value.emplace_back("velocity", vector_value(record.velocity));
      value.emplace_back("motionVelocity", vector_value(record.motion_velocity));
      value.emplace_back("gaze", vector_value(record.look_direction));
      value.emplace_back("horseId", Value(std::int64_t(record.horse_id)));
      value.emplace_back("inAir", Value(record.posture.in_air));
      value.emplace_back("crouched", Value(record.posture.crouched));
      value.emplace_back("stance", Value(std::int64_t(record.posture.stance)));
      value.emplace_back("guard", Value(record.posture.guard));
      value.emplace_back("guardUp", Value(record.posture.raised));
      value.emplace_back("fightZone", Value(std::int64_t(record.posture.combat_zone)));
      value.emplace_back("physicsProfile", Value(std::int64_t(record.posture.physics_profile)));
      value.emplace_back("equipment", equipment_value(record.outfit));
      value.emplace_back("hands", hands_value(record.hands));
    }
    return Value(std::move(value));
  }
  static Value id_list(const std::vector<core::EntityId>& ids, std::size_t begin, std::size_t end) {
    Value::Array values;
    values.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index)
      values.emplace_back(std::int64_t(ids[index]));
    return Value(std::move(values));
  }
  // IPC frame sizes: records per entityRecords message and ids per list of one message.
  static constexpr std::size_t kRecordsPerMessage = 64;
  static constexpr std::size_t kIdsPerMessage = 1024;
  // Report slices per cycle: every member's view is compared once per ~100 ms, spread over the
  // ticks of that interval so no single tick carries every peer.
  unsigned report_slices() const {
    const double slices = std::round(settings_.tick_rate_hz * 0.1);
    if (slices < 1.0)
      return 1;
    return unsigned(slices);
  }
  // Players' records carry their outfit and stats, so a full message can pass the host's limit:
  // such a message goes in two halves. A lost record would refuse every view that names it.
  void send_entity_records(const std::vector<resources::ReportedEntity>& records, std::size_t begin,
                           std::size_t end) {
    Value::Array values;
    values.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index)
      values.push_back(entity_record_value(records[index]));
    Value message(
        Value::Map{{"op", Value("entityRecords")}, {"entities", Value(std::move(values))}});
    if (end - begin > 1 && sdk::encode(message).size() > ScriptProcess::kMaxMessageBytes) {
      const std::size_t middle = begin + (end - begin) / 2;
      send_entity_records(records, begin, middle);
      send_entity_records(records, middle, end);
      return;
    }
    scripts_->send(message);
  }
  void send_view_delta(const resources::PeerViewDelta& delta) {
    // Large changes travel in several parts; the resource host applies owned/localPlayerId and
    // validates the view on the final part. Every part of one delta is sent before the pump.
    const std::size_t added_parts = (delta.added.size() + kIdsPerMessage - 1) / kIdsPerMessage;
    const std::size_t removed_parts = (delta.removed.size() + kIdsPerMessage - 1) / kIdsPerMessage;
    const std::size_t parts = std::max<std::size_t>(1, std::max(added_parts, removed_parts));
    for (std::size_t part = 0; part < parts; ++part) {
      const std::size_t first = part * kIdsPerMessage;
      const std::size_t added_begin = std::min(first, delta.added.size());
      const std::size_t added_end = std::min(first + kIdsPerMessage, delta.added.size());
      const std::size_t removed_begin = std::min(first, delta.removed.size());
      const std::size_t removed_end = std::min(first + kIdsPerMessage, delta.removed.size());
      const bool final = part + 1 == parts;
      Value::Map message{{"op", Value("entityViewDelta")},
                         {"peer", Value(std::int64_t(delta.peer))},
                         {"localPlayerId", Value(std::int64_t(delta.local_player))},
                         {"added", id_list(delta.added, added_begin, added_end)},
                         {"removed", id_list(delta.removed, removed_begin, removed_end)},
                         {"final", Value(final)}};
      if (final)
        message.emplace_back("owned", id_list(delta.owned, 0, delta.owned.size()));
      scripts_->send(Value(std::move(message)));
    }
  }
  // Reports the views of this tick's slice of members as changed-state deltas: records of
  // entities that changed (once per cycle), then per peer the members that entered or left.
  // Every connected player's round-trip time in milliseconds (player.ping), every
  // kPingReportSeconds: [[id, ms], ...].
  static constexpr double kPingReportSeconds = 2.0;
  double pings_reported_at_ = -kPingReportSeconds;
  void report_pings() {
    const double now = clock_seconds();
    if (now - pings_reported_at_ < kPingReportSeconds) {
      return;
    }
    pings_reported_at_ = now;
    Value::Array pings;
    for (const auto& [key, member] : members_) {
      (void)key;
      const double milliseconds = std::round(server_.peer_rtt_seconds(member.peer) * 1000.0);
      pings.push_back(Value(
          Value::Array{Value(std::int64_t(member.avatar)), Value(std::int64_t(milliseconds))}));
    }
    scripts_->send(
        Value(Value::Map{{"op", Value("playerPings")}, {"pings", Value(std::move(pings))}}));
  }
  // The snapshot builds get what the tick has left after its other work (receiving, sending, the
  // resources), with a margin: with 500 players that work alone took half the tick, the builds
  // were allowed 8 ms on top, and the tick missed every third deadline, so snapshots went out
  // unevenly. The other work is averaged over about a second, so one slow tick does not move it.
  void plan_snapshot_builds(double tick_seconds) {
    constexpr double kTickShare = 0.85;
    constexpr double kAveraging = 0.02;
    const double period = 1.0 / settings_.tick_rate_hz;
    // A stalled tick (the scripts held for seconds while a crowd joins) counts as one full tick.
    const double other =
        std::clamp(tick_seconds - server_.last_build_seconds(), 0.0, std::max(period, 0.0));
    other_tick_seconds_ = other_tick_seconds_ < 0.0
                              ? other
                              : other_tick_seconds_ + (other - other_tick_seconds_) * kAveraging;
    server_.set_snapshot_build_target(period * kTickShare - other_tick_seconds_);
  }
  void report_entity_views() {
    const unsigned slices = report_slices();
    const unsigned slice = unsigned(server_.world().tick() % slices);
    if (slice == 0) {
      const auto removed = view_reports_.begin_cycle();
      for (std::size_t offset = 0; offset < removed.size(); offset += kIdsPerMessage) {
        const auto end = std::min(offset + kIdsPerMessage, removed.size());
        scripts_->send(Value(
            Value::Map{{"op", Value("entityRemoved")}, {"ids", id_list(removed, offset, end)}}));
      }
    }
    std::vector<resources::ReportedEntity> upserts;
    std::vector<resources::PeerViewDelta> deltas;
    // The known entities' records, each in its own slice of the cycle.
    for (const auto id : view_reports_.due_in_slice(slices, slice)) {
      if (const auto record = entity_record(id)) {
        if (view_reports_.refresh(*record))
          upserts.push_back(*record);
      }
    }
    for (auto& [key, member] : members_) {
      (void)key;
      if (member.report_slice % slices != slice)
        continue;
      // The same visible set and owners as the last complete report: nothing to compare or send.
      // Most of a standing crowd's views stay the same from one cycle to the next.
      const std::uint64_t view_revision = server_.view_revision(member.peer);
      const std::uint64_t ownership = server_.ownership_revision();
      if (view_revision != 0 && view_revision == member.reported_view_revision &&
          ownership == member.reported_ownership && member.avatar == member.reported_avatar) {
        continue;
      }
      member.reported_view_revision = 0;
      auto visible = server_.visible_entities(member.peer);
      if (std::find(visible.begin(), visible.end(), member.avatar) == visible.end())
        visible.push_back(member.avatar);
      if (visible.size() > resources::kMaxViewEntities) {
        std::cerr << "RESOURCE entity view exceeds SDK limit" << std::endl;
        continue;
      }
      resources::PeerView view;
      view.peer = member.peer;
      view.local_player = member.avatar;
      view.members.reserve(visible.size());
      bool complete = true;
      for (auto id : visible) {
        // A new one at once, so the view can name it; a known one in its own slice (above).
        if (!view_reports_.known(id)) {
          const auto record = entity_record(id);
          if (!record) {
            complete = false;
            continue;
          }
          if (view_reports_.refresh(*record))
            upserts.push_back(*record);
        }
        const auto* slot = server_.world().slot(id);
        if (!slot) {
          complete = false;
          continue;
        }
        view.members.push_back({id, slot->owner == member.peer});
      }
      resources::PeerViewDelta delta;
      if (view_reports_.update_view(std::move(view), delta))
        deltas.push_back(std::move(delta));
      if (complete) {
        member.reported_view_revision = view_revision;
        member.reported_ownership = ownership;
        member.reported_avatar = member.avatar;
      }
    }
    for (std::size_t offset = 0; offset < upserts.size(); offset += kRecordsPerMessage) {
      send_entity_records(upserts, offset, std::min(offset + kRecordsPerMessage, upserts.size()));
    }
    for (const auto& delta : deltas)
      send_view_delta(delta);
    // Player and horse reports follow the same slices; unchanged state sends nothing. Each
    // hundred players more makes a player's cycle one ~100 ms longer: a full record of every
    // player of a 500-player crowd ten times a second held the scripts for seconds.
    const unsigned player_slices =
        slices * unsigned(std::max<std::size_t>(1, (members_.size() + 99) / 100));
    const unsigned player_slice = unsigned(server_.world().tick() % player_slices);
    for (const auto& [key, member] : members_) {
      (void)key;
      if (member.report_slice % player_slices == player_slice)
        report_peer_player(member.avatar, member.peer);
    }
    if (slice == 0) {
      for (const auto& [script, id] : horses_)
        report_horse(script, id);
      for (const auto& [script, id] : npcs_) {
        report_npc(script, id);
      }
    }
  }
  void tick_resources() {
    auto submissions = server_.take_chat();
    auto script_submissions = server_.take_scripts();
    // Taken every tick: without a resource host there are no resource NPCs to tell of.
    const auto npc_simulator_changes = server_.take_npc_simulator_changes();
    const auto npc_hits = server_.take_npc_hits();
    const auto npc_intent_dones = server_.take_npc_intent_dones();
    const auto npc_animation_ends = server_.take_npc_animation_ends();
    const auto npc_uses = server_.take_npc_uses();
    const auto npc_engages = server_.take_npc_engages();
    const auto horse_health_changes = server_.take_horse_health_changes();
    const auto horse_stamina_changes = server_.take_horse_stamina_changes();
    if (!scripts_) {
      for (const auto& e : submissions)
        server_.send_chat(e.player, {"Server", "Resource host is not running.", 0});
      return;
    }
    report_entity_views();
    report_pings();
    report_npc_simulators(npc_simulator_changes);
    report_npc_hits(npc_hits);
    report_npc_intent_dones(npc_intent_dones);
    report_npc_animation_ends(npc_animation_ends);
    report_npc_uses(npc_uses);
    report_npc_engages(npc_engages);
    report_horse_health_changes(horse_health_changes);
    report_horse_stamina_changes(horse_stamina_changes);
    scripts_->send(Value(Value::Map{{"op", Value("entityPump")}}));
    for (auto& [player, nickname] : server_.take_nicknames()) {
      if (server_.world().slot(player)) {
        server_.world().write(player, Nickname, std::move(nickname));
        report_player(player);
      }
    }
    for (auto& [player, gain] : server_.take_xp_gains()) {
      if (!server_.world().slot(player))
        continue;
      scripts_->send(Value(Value::Map{{"op", Value("xpGaining")},
                                      {"id", Value(std::int64_t(player))},
                                      {"track", Value(std::int64_t(gain.track))},
                                      {"xp", Value(double(gain.xp) / integration::kXpUnits)},
                                      {"multipliers", Value(gain.multipliers)},
                                      {"source", Value(gain.source)}}));
    }
    for (const auto& [player, buffs] : server_.take_buff_reports()) {
      if (!server_.world().slot(player))
        continue;
      Value::Array entries;
      for (const auto& entry : buffs) {
        // Instance ids are 64-bit: as decimal text, which a resource host's numbers cannot spoil.
        entries.push_back(
            Value(Value::Array{Value(integration::guid_text(entry.buff)),
                               Value(std::to_string(entry.instance)), Value(entry.server)}));
      }
      scripts_->send(Value(Value::Map{{"op", Value("playerBuffs")},
                                      {"id", Value(std::int64_t(player))},
                                      {"buffs", Value(std::move(entries))}}));
    }
    for (const auto& [player, opponent] : server_.take_player_attacks()) {
      scripts_->send(Value(Value::Map{{"op", Value("playerAttack")},
                                      {"id", Value(std::int64_t(player))},
                                      {"opponent", Value(std::int64_t(opponent))}}));
    }
    // Blows held for a ruling: the resource host raises playerHit and answers.
    // Harm the players' games hold for a ruling (playerHarm); the verdict comes back as hitVerdict.
    for (const auto& [player, harm] : server_.take_player_harms()) {
      scripts_->send(Value(Value::Map{{"op", Value("playerHarm")},
                                      {"id", Value(std::int64_t(player))},
                                      {"request", Value(std::int64_t(harm.request))},
                                      {"amount", Value(double(harm.amount))},
                                      {"reason", Value(std::int64_t(harm.reason))},
                                      {"subpart", Value(std::int64_t(harm.subpart))},
                                      {"attacker", Value(std::int64_t(harm.attacker))}}));
    }
    for (const auto& [player, hit] : server_.take_player_hits()) {
      Value::Map report{{"op", Value("playerHit")},
                        {"id", Value(std::int64_t(player))},
                        {"request", Value(std::int64_t(hit.request))},
                        {"attacker", Value(std::int64_t(hit.attacker))},
                        {"damage", Value(double(hit.damage))},
                        {"stamina", Value(double(hit.stamina))}};
      if (hit.armed) {
        report.emplace_back("weapon", Value(integration::item_class_script_text(hit.weapon)));
      }
      if (hit.missile) {
        report.emplace_back("missile", Value(true));
      }
      if (hit.priced) {
        const auto passes = [](const std::array<float, 3>& values) {
          return Value(Value::Array{Value(double(values[0])), Value(double(values[1])),
                                    Value(double(values[2]))});
        };
        report.emplace_back(
            "price",
            Value(Value::Map{{"attack", passes(hit.price.attack)},
                             {"armor", passes(hit.price.armor)},
                             {"blockDefense", Value(double(hit.price.block_defense))},
                             {"bodyPartCoefficient", Value(double(hit.price.body_coefficient))},
                             {"subpart", Value(std::int64_t(hit.price.subpart))},
                             {"blocked", Value(hit.price.blocked)},
                             {"perfectBlock", Value(hit.price.perfect_block)},
                             {"blockBroken", Value(hit.price.block_broken)}}));
      }
      scripts_->send(Value(std::move(report)));
    }
    for (const auto& [player, damage] : server_.take_player_damage()) {
      scripts_->send(Value(Value::Map{{"op", Value("playerDamage")},
                                      {"id", Value(std::int64_t(player))},
                                      {"attacker", Value(std::int64_t(damage.attacker))},
                                      {"amount", Value(double(damage.amount))},
                                      {"reason", Value(std::int64_t(damage.reason))},
                                      {"subpart", Value(std::int64_t(damage.subpart))}}));
    }
    // The level's items players' games would take up: the resource host decides.
    for (const auto& [player, key] : server_.take_world_pickups()) {
      if (!server_.world().slot(player))
        continue;
      scripts_->send(Value(Value::Map{
          {"op", Value("worldPickup")}, {"id", Value(std::int64_t(player))}, {"key", Value(key)}}));
    }
    // Where the rigid props players' games simulate are: the resource host checks it.
    for (const auto& [player, report] : server_.take_prop_reports()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("propReport")},
                                      {"id", Value(std::int64_t(player))},
                                      {"prop", Value(std::int64_t(report.id))},
                                      {"position", point_value(report.position)},
                                      {"rotation", quaternion_value(report.rotation)},
                                      {"resting", Value(report.resting)},
                                      {"hitSpeed", Value(double(report.hit_speed))}}));
    }
    // What players' games did with carts: the resource host rules on it.
    for (const auto& [player, report] : server_.take_cart_reports()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      const char* op = report.kind == integration::CartReportKind::Enter     ? "cartEnter"
                       : report.kind == integration::CartReportKind::Leave   ? "cartLeave"
                       : report.kind == integration::CartReportKind::AtStart ? "cartAtStart"
                                                                             : "cartDrive";
      Value::Map message{{"op", Value(op)},
                         {"id", Value(std::int64_t(player))},
                         {"cart", Value(std::int64_t(report.cart))}};
      if (report.kind == integration::CartReportKind::Enter) {
        message.emplace_back("seat", Value(integration::cart_seat_name(report.seat)));
      }
      if (report.kind == integration::CartReportKind::Drive) {
        message.emplace_back("position", point_value(report.position));
        message.emplace_back("rotation", quaternion_value(report.rotation));
        message.emplace_back("pace", Value(integration::cart_pace_name(report.pace)));
      }
      scripts_->send(Value(std::move(message)));
    }
    // Where players' shots landed: the resource host lays each as a ground stack.
    for (const auto& [player, landed] : server_.take_shot_landings()) {
      if (!server_.world().slot(player))
        continue;
      Value::Map message{
          {"op", Value("shotLanded")},
          {"id", Value(std::int64_t(player))},
          {"item", Value(integration::guid_text(landed.ammo))},
          {"position",
           vector_value(core::Vec3{landed.position[0], landed.position[1], landed.position[2]})},
          {"rotation", Value(Value::Map{{"x", Value(double(landed.rotation[0]))},
                                        {"y", Value(double(landed.rotation[1]))},
                                        {"z", Value(double(landed.rotation[2]))},
                                        {"w", Value(double(landed.rotation[3]))}})},
          {"stuck", Value(landed.stuck)}};
      // What it struck there (playerMissileHit), as a raycast hit.
      if (landed.hit) {
        message.emplace_back("hit", ray_hit_value(*landed.hit));
      }
      scripts_->send(Value(std::move(message)));
    }
    // Thieves' requests and results: the resource host decides and answers.
    for (const auto& [player, report] : server_.take_pickpocket_reports()) {
      if (!server_.world().slot(player))
        continue;
      static constexpr const char* kKinds[] = {"", "request", "caught", "seen", "finished"};
      scripts_->send(Value(Value::Map{{"op", Value("pickpocket")},
                                      {"id", Value(std::int64_t(player))},
                                      {"kind", Value(kKinds[std::size_t(report.kind)])},
                                      {"victim", Value(std::int64_t(report.victim))}}));
    }
    // The doors the players' games see, their own use of them and the answers to nearest-door
    // questions: the resource host keeps the state and tells the others.
    for (const auto& [player, doors] : server_.take_door_reports()) {
      if (!server_.world().slot(player))
        continue;
      Value::Array list;
      for (const integration::DoorEntry& door : doors) {
        list.emplace_back(door_entry_value(door));
      }
      scripts_->send(Value(Value::Map{{"op", Value("doorReport")},
                                      {"id", Value(std::int64_t(player))},
                                      {"doors", Value(std::move(list))}}));
    }
    for (const auto& [player, use] : server_.take_door_uses()) {
      if (!server_.world().slot(player))
        continue;
      static constexpr const char* kActions[] = {"open", "close", "unlock", "lockpick"};
      scripts_->send(Value(Value::Map{{"op", Value("doorUse")},
                                      {"id", Value(std::int64_t(player))},
                                      {"guid", Value(integration::door_guid_text(use.guid))},
                                      {"action", Value(kActions[std::size_t(use.action)])},
                                      {"keySide", Value(use.keySide)},
                                      {"npc", Value(std::int64_t(use.npc))}}));
    }
    for (const auto& [player, reply] : server_.take_door_replies()) {
      if (!server_.world().slot(player))
        continue;
      Value::Map message{{"op", Value("doorNearestReply")},
                         {"id", Value(std::int64_t(player))},
                         {"request", Value(std::int64_t(reply.request))}};
      if (reply.found) {
        message.emplace_back("door", door_entry_value(reply.door));
      } else {
        message.emplace_back("reason", Value(reply.reason));
      }
      scripts_->send(Value(std::move(message)));
    }
    // What the players' games met along the rays, or found around the points, the resource host
    // asked for.
    for (const auto& [player, reply] : server_.take_world_ray_replies()) {
      if (!server_.world().slot(player))
        continue;
      static constexpr const char* kStatuses[] = {"miss", "hit", "unavailable"};
      const bool bodies = reply.query == integration::RayQuery::Bodies;
      Value::Map message{{"op", Value(bodies ? "worldBodiesReply" : "worldRaycastReply")},
                         {"id", Value(std::int64_t(player))},
                         {"request", Value(std::int64_t(reply.request))},
                         {"status", Value(kStatuses[std::size_t(reply.status)])}};
      Value::Array found;
      for (const integration::WorldRayHit& hit : reply.hits) {
        found.emplace_back(ray_hit_value(hit));
      }
      for (const integration::WorldBody& body : reply.bodies) {
        found.emplace_back(Value::Map{{"entity", Value(std::int64_t(body.entity))},
                                      {"position", point_value(body.position)}});
      }
      message.emplace_back(bodies ? "bodies" : "hits", Value(std::move(found)));
      scripts_->send(Value(std::move(message)));
    }
    // Camera shots that ended on a player's game without the resource host clearing them.
    for (const auto& [player, reason] : server_.take_camera_shot_ends()) {
      if (!server_.world().slot(player))
        continue;
      scripts_->send(
          Value(Value::Map{{"op", Value("cameraShotEnded")},
                           {"id", Value(std::int64_t(player))},
                           {"reason", Value(integration::camera_shot_end_name(reason))}}));
    }
    // Pick-ups and let-gos of players' bodies: the resource host decides and tells.
    for (const auto& [player, report] : server_.take_body_carry_reports()) {
      if (!server_.world().slot(player))
        continue;
      scripts_->send(Value(Value::Map{
          {"op", Value("bodyCarryReport")},
          {"id", Value(std::int64_t(player))},
          {"kind",
           Value(report.kind == integration::BodyCarryReport::Kind::Started ? "started" : "ended")},
          {"carried", Value(std::int64_t(report.carried))}}));
    }
    // The graves the players' levels have and the ones they dug out: the resource
    // host keeps their state and tells everyone.
    for (const auto& [player, grave] : server_.take_graves()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      const core::Vec3 at{float(grave.position[0]) / 100.0f, float(grave.position[1]) / 100.0f,
                          float(grave.position[2]) / 100.0f};
      const bool dug = grave.kind == integration::GraveKind::Dug;
      scripts_->send(Value(Value::Map{{"op", Value(dug ? "graveDug" : "graveReport")},
                                      {"id", Value(std::int64_t(player))},
                                      {"grave", Value(integration::grave_id(grave.position))},
                                      {"position", vector_value(at)},
                                      {"dug", Value(grave.dug)}}));
    }
    // What the players' swings met that is not a body: the resource host raises
    // playerWeaponHit.
    for (const auto& [player, hit] : server_.take_weapon_hits()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("playerWeaponHit")},
                                      {"id", Value(std::int64_t(player))},
                                      {"hit", ray_hit_value(hit)}}));
    }
    // How the animation requests the server ordered ended in the players' games: the
    // resource host raises playerAnimationEnd, once per request.
    for (const auto& [player, end] : server_.take_animation_ends()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      static constexpr const char* kReasons[] = {"",        "finished", "interrupted",
                                                 "refused", "stopped",  "replaced"};
      static constexpr const char* kExits[] = {"", "StandUp", "GetUp"};
      scripts_->send(Value(Value::Map{{"op", Value("animationEnd")},
                                      {"id", Value(std::int64_t(player))},
                                      {"request", Value(std::int64_t(end.request))},
                                      {"reason", Value(kReasons[std::size_t(end.reason)])},
                                      {"exit", Value(kExits[std::size_t(end.exit)])}}));
    }
    // The props players' animations took into a hand or put down: the resource host
    // raises propPickedUp and propPlaced.
    for (const auto& [player, cue] : server_.take_animation_prop_cues()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("animationPropCue")},
                                      {"id", Value(std::int64_t(player))},
                                      {"request", Value(std::int64_t(cue.request))},
                                      {"prop", Value(std::int64_t(cue.prop))},
                                      {"taken", Value(cue.taken)}}));
    }
    // What the players' clients said about a pillory: the resource host raises
    // playerPilloryReleased when a lock pick opened one (a lock opened in a player's game
    // carries the lock's name).
    for (const auto& [player, report] : server_.take_pillory_releases()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("pilloryReleased")},
                                      {"id", Value(std::int64_t(player))},
                                      {"reason", Value(std::int64_t(report.reason))},
                                      {"lock", Value(report.lock)}}));
    }
    // What server NPCs said on their simulators: the resource host shows it to everyone.
    for (auto& [simulator, bark] : server_.take_npc_barks()) {
      scripts_->send(Value(Value::Map{{"op", Value("npcBarked")},
                                      {"id", Value(std::int64_t(bark.npc))},
                                      {"from", Value(std::int64_t(simulator))},
                                      {"localize", Value(bark.localize)},
                                      {"text", Value(bark.text)},
                                      {"voice", Value(bark.voice)},
                                      {"purpose", Value(std::int64_t(bark.purpose))}}));
    }
    tick_voice(clock_seconds());
    // The players who whistled: the resource host tells the players near them.
    for (const auto player : server_.take_whistles()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(
          Value(Value::Map{{"op", Value("playerWhistled")}, {"id", Value(std::int64_t(player))}}));
    }
    // Where the fast travels players began go: the resource host decides whether they lead to
    // another of the server's maps (maps/level_travel.cjs).
    for (const auto& [player, target] : server_.take_travel_targets()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("playerTravelStarted")},
                                      {"id", Value(std::int64_t(player))},
                                      {"x", Value(double(target.x))},
                                      {"y", Value(double(target.y))},
                                      {"z", Value(double(target.z))}}));
    }
    // The world interactions the players used: the resource host runs the handlers.
    for (const auto& [player, interaction] : server_.take_interactions_fired()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("interactionFired")},
                                      {"id", Value(std::int64_t(player))},
                                      {"interaction", Value(std::int64_t(interaction))}}));
    }
    // What players ask of siege engines and where the shots they report came down:
    // the resource host rules on both (siege/engines.cjs use and impactReport).
    for (const auto& [player, use] : server_.take_siege_uses()) {
      if (!server_.world().slot(player))
        continue;
      static constexpr const char* kActions[] = {"operate", "leave", "load", "fire"};
      Value::Map message{{"op", Value("siegeUse")},
                         {"id", Value(std::int64_t(player))},
                         {"engine", Value(std::int64_t(use.engine))},
                         {"action", Value(kActions[std::size_t(use.action)])}};
      if (use.has_look) {
        message.emplace_back("look",
                             vector_value(core::Vec3{use.look[0], use.look[1], use.look[2]}));
      }
      scripts_->send(Value(std::move(message)));
    }
    for (const auto& [player, impact] : server_.take_siege_impacts()) {
      if (!server_.world().slot(player))
        continue;
      scripts_->send(Value(Value::Map{
          {"op", Value("siegeImpactReport")},
          {"id", Value(std::int64_t(player))},
          {"engine", Value(std::int64_t(impact.engine))},
          {"shot", Value(std::int64_t(impact.shot))},
          {"position",
           vector_value(core::Vec3{impact.position[0], impact.position[1], impact.position[2]})}}));
    }
    // The events of the players' prop placements: the resource host decides on them.
    for (const auto& [player, report] : server_.take_placer_reports()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(placer_report_value(player, report));
    }
    // What players did with siege ladders and stone piles: the resource host rules on
    // all of it (siege/ladders.cjs use and settled, siege/stones.cjs take and impact).
    for (const auto& [player, report] : server_.take_siege_ladder_reports()) {
      if (!server_.world().slot(player))
        continue;
      if (report.kind == integration::LadderMessageKind::Use) {
        scripts_->send(Value(Value::Map{
            {"op", Value("siegeLadderUse")},
            {"id", Value(std::int64_t(player))},
            {"ladder", Value(std::int64_t(report.ladder))},
            {"action",
             Value(report.action == integration::LadderAction::Push ? "push" : "raise")}}));
      } else {
        scripts_->send(Value(Value::Map{
            {"op", Value("siegeLadderSettled")},
            {"id", Value(std::int64_t(player))},
            {"ladder", Value(std::int64_t(report.ladder))},
            {"pose", Value(report.pose == integration::LadderPose::Down ? "down" : "standing")}}));
      }
    }
    for (const auto& [player, report] : server_.take_stone_pile_reports()) {
      if (!server_.world().slot(player))
        continue;
      if (report.kind == integration::StonePileMessageKind::Take) {
        scripts_->send(Value(Value::Map{{"op", Value("stonePileTake")},
                                        {"id", Value(std::int64_t(player))},
                                        {"pile", Value(std::int64_t(report.pile))}}));
      } else {
        scripts_->send(Value(
            Value::Map{{"op", Value("stoneImpact")},
                       {"id", Value(std::int64_t(player))},
                       {"pile", Value(std::int64_t(report.pile))},
                       {"position", vector_value(core::Vec3{report.position[0], report.position[1],
                                                            report.position[2]})}}));
      }
    }
    // Where the items carries let go of lie: the resource host lays carryable props.
    for (const auto& [player, put_down] : server_.take_carry_put_downs()) {
      if (!server_.world().slot(player))
        continue;
      scripts_->send(Value(Value::Map{
          {"op", Value("carryPutDown")},
          {"id", Value(std::int64_t(player))},
          {"item", Value(integration::guid_text(put_down.item))},
          {"position", vector_value(core::Vec3{put_down.position[0], put_down.position[1],
                                               put_down.position[2]})}}));
    }
    // Containers' locks players opened in their games, before the opens that follow
    // them in the inventory reports.
    for (const auto& [player, unlock] : server_.take_stash_unlocks()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(
          Value::Map{{"op", Value("stashUnlock")},
                     {"id", Value(std::int64_t(player))},
                     {"stash", Value(double(unlock.stash))},
                     {"method", Value(integration::stash_unlock_method_name(unlock.method))}}));
    }
    // What each player's game build gave their client: the functions it found and lost.
    for (auto& [player, natives] : server_.take_client_natives()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      if (!natives.missing.empty() || !natives.combat || !natives.riding || !natives.inventory) {
        std::cout << "CLIENT player=" << player << " found=" << natives.found
                  << " missing=" << natives.missing.size() << (natives.combat ? "" : " combat=off")
                  << (natives.riding ? "" : " riding=off")
                  << (natives.inventory ? "" : " inventory=off") << std::endl;
      }
      Value::Array missing;
      for (auto& name : natives.missing) {
        missing.push_back(Value(std::move(name)));
      }
      scripts_->send(Value(Value::Map{{"op", Value("clientNatives")},
                                      {"id", Value(std::int64_t(player))},
                                      {"found", Value(double(natives.found))},
                                      {"combat", Value(natives.combat)},
                                      {"riding", Value(natives.riding)},
                                      {"inventory", Value(natives.inventory)},
                                      {"missing", Value(std::move(missing))}}));
    }
    // Quests players started or stopped following in their games.
    for (const auto& [player, tracking] : server_.take_quest_tracking()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("questTracking")},
                                      {"id", Value(std::int64_t(player))},
                                      {"quest", Value(double(tracking.quest))},
                                      {"tracked", Value(tracking.tracked)}}));
    }
    // Rows players picked in conversations and conversations they ended: the
    // resource host checks them against its sessions (dialogue/dialogues.cjs).
    for (const auto& [player, choice] : server_.take_dialogue_choices()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("dialogueChoice")},
                                      {"id", Value(std::int64_t(player))},
                                      {"session", Value(std::int64_t(choice.session))},
                                      {"generation", Value(std::int64_t(choice.generation))},
                                      {"index", Value(std::int64_t(choice.index))}}));
    }
    for (const auto& [player, cancel] : server_.take_dialogue_cancels()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("dialogueCancel")},
                                      {"id", Value(std::int64_t(player))},
                                      {"session", Value(std::int64_t(cancel.session))},
                                      {"reason", Value(std::int64_t(cancel.reason))}}));
    }
    // What players' games did at crafting stations: the resource host keeps
    // the sessions (crafting/crafting.cjs) and answers an ask with craftVerdict.
    for (const auto& [player, report] : server_.take_craft_reports()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      const char* kind = report.kind == integration::CraftReport::Kind::Ask       ? "ask"
                         : report.kind == integration::CraftReport::Kind::Started ? "started"
                         : report.kind == integration::CraftReport::Kind::Recipe  ? "recipe"
                         : report.kind == integration::CraftReport::Kind::Failed  ? "failed"
                         : report.kind == integration::CraftReport::Kind::Prepare ? "prepare"
                                                                                  : "ended";
      const char* craft = report.craft == integration::CraftKind::Smithing ? "smithing" : "alchemy";
      scripts_->send(Value(Value::Map{
          {"op", Value("craftReport")},
          {"id", Value(std::int64_t(player))},
          {"report", Value(std::string(kind))},
          {"request", Value(std::int64_t(report.request))},
          {"kind", Value(std::string(craft))},
          {"station",
           vector_value(core::Vec3{report.station[0], report.station[1], report.station[2]})},
          {"reason", Value(report.reason)}}));
    }
    // What lies on the alchemy tables players brew at: crafting/crafting.cjs keeps it
    // for Crafting.session's resources.
    for (const auto& [player, table] : server_.take_craft_tables()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      Value::Array items;
      for (const auto& item : table.items) {
        items.push_back(Value(Value::Array{
            Value(std::int64_t(item.position)), Value(integration::guid_text(item.item)),
            Value(item.base), Value(item.milled), Value(item.distilled)}));
      }
      scripts_->send(Value(Value::Map{{"op", Value("craftTable")},
                                      {"id", Value(std::int64_t(player))},
                                      {"items", Value(std::move(items))}}));
    }
    // Herbs players' games picked: the resource host decides whether each stands and
    // gives the herb (gathering/gathering.cjs), answering with gatherVerdict.
    for (const auto& [player, report] : server_.take_gather_reports()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{
          {"op", Value("gatherReport")},
          {"id", Value(std::int64_t(player))},
          {"request", Value(std::int64_t(report.request))},
          {"item", Value(integration::guid_text(report.item))},
          {"amount", Value(std::int64_t(report.amount))},
          {"kind", Value(std::int64_t(report.kind))},
          {"position",
           vector_value(core::Vec3{report.position[0], report.position[1], report.position[2]})},
          {"radius", Value(double(report.radius))},
          {"plants", Value(std::int64_t(report.plants))}}));
    }
    // Moves players made in their dice matches: the resource host checks them against
    // the matches it runs (dice/dice.cjs).
    for (const auto& [player, move] : server_.take_dice_moves()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      const char* kind = move.kind == integration::DiceMove::Kind::Continue     ? "continue"
                         : move.kind == integration::DiceMove::Kind::Pass       ? "pass"
                         : move.kind == integration::DiceMove::Kind::GaveUp     ? "gaveUp"
                         : move.kind == integration::DiceMove::Kind::Throw      ? "throw"
                         : move.kind == integration::DiceMove::Kind::Select     ? "select"
                         : move.kind == integration::DiceMove::Kind::TableUse   ? "tableUse"
                         : move.kind == integration::DiceMove::Kind::BadgePass  ? "badgePass"
                         : move.kind == integration::DiceMove::Kind::Badge      ? "badge"
                         : move.kind == integration::DiceMove::Kind::BustChoice ? "bustChoice"
                                                                                : "failed";
      scripts_->send(Value(Value::Map{{"op", Value("diceMove")},
                                      {"id", Value(std::int64_t(player))},
                                      {"match", Value(std::int64_t(move.match))},
                                      {"kind", Value(std::string(kind))},
                                      {"held", Value(std::int64_t(move.held))}}));
    }
    // Where the dice table's things are in the throwers' games: dice/dice.cjs passes
    // them on to the players watching. A thing is [key as 16 hex digits, x, y, z, qx, qy, qz, qw,
    // shown, scale, model, material].
    for (const auto& [player, view] : server_.take_dice_views()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      Value::Array things;
      for (const auto& thing : view.things) {
        char key[17] = {};
        std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(thing.key));
        Value::Array cells{Value(std::string(key))};
        for (const float value : thing.position) {
          cells.push_back(Value(double(value)));
        }
        for (const float value : thing.rotation) {
          cells.push_back(Value(double(value)));
        }
        cells.push_back(Value(thing.shown));
        cells.push_back(Value(double(thing.scale)));
        cells.push_back(Value(thing.model));
        cells.push_back(Value(thing.material));
        things.push_back(Value(std::move(cells)));
      }
      scripts_->send(Value(Value::Map{{"op", Value("diceView")},
                                      {"id", Value(std::int64_t(player))},
                                      {"match", Value(std::int64_t(view.match))},
                                      {"things", Value(std::move(things))}}));
    }
    // Baskets players confirmed on vendor screens and screens that went away:
    // the resource host settles them against its vendors (vendors/vendors.cjs). A line is
    // [row, units].
    for (const auto& [player, trade] : server_.take_vendor_trades()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      const auto lines_of = [](const std::vector<integration::VendorTradeLine>& lines) {
        Value::Array list;
        for (const integration::VendorTradeLine& line : lines) {
          list.push_back(Value(Value::Array{Value(line.row), Value(std::int64_t(line.units))}));
        }
        return Value(std::move(list));
      };
      scripts_->send(Value(Value::Map{{"op", Value("vendorTrade")},
                                      {"id", Value(std::int64_t(player))},
                                      {"session", Value(std::int64_t(trade.session))},
                                      {"revision", Value(double(trade.revision))},
                                      {"bought", lines_of(trade.bought)},
                                      {"sold", lines_of(trade.sold)}}));
    }
    // Prices players offered in the haggle of a vendor that haggles (0.4.1): the resource host
    // answers each with vendorHaggleReply. A price is what the player pays; negative, the vendor.
    for (const auto& [player, haggle] : server_.take_vendor_haggles()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      const auto lines_of = [](const std::vector<integration::VendorTradeLine>& lines) {
        Value::Array list;
        for (const integration::VendorTradeLine& line : lines) {
          list.push_back(Value(Value::Array{Value(line.row), Value(std::int64_t(line.units))}));
        }
        return Value(std::move(list));
      };
      scripts_->send(Value(Value::Map{{"op", Value("vendorHaggle")},
                                      {"id", Value(std::int64_t(player))},
                                      {"session", Value(std::int64_t(haggle.session))},
                                      {"revision", Value(double(haggle.revision))},
                                      {"round", Value(std::int64_t(haggle.round))},
                                      {"offered", Value(std::int64_t(haggle.offered))},
                                      {"bought", lines_of(haggle.bought)},
                                      {"sold", lines_of(haggle.sold)}}));
    }
    for (const auto& [player, closed] : server_.take_vendor_closes()) {
      if (!server_.world().slot(player)) {
        continue;
      }
      scripts_->send(Value(Value::Map{{"op", Value("vendorClosed")},
                                      {"id", Value(std::int64_t(player))},
                                      {"session", Value(std::int64_t(closed.session))},
                                      {"reason", Value(std::int64_t(closed.reason))}}));
    }
    for (auto& [player, report] : server_.take_inventory_reports()) {
      if (!server_.world().slot(player))
        continue;
      if (report.kind == integration::InventoryReport::Kind::Shown) {
        scripts_->send(Value(Value::Map{{"op", Value("inventoryShown")},
                                        {"id", Value(std::int64_t(player))},
                                        {"revision", Value(double(report.revision))}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::Changes) {
        Value::Array changes;
        for (const auto& change : report.changes) {
          changes.push_back(Value(
              Value::Array{Value(double(change.kind)), Value(change.id),
                           Value(integration::guid_text(change.item)), Value(double(change.amount)),
                           Value(double(change.quality)), Value(double(change.health))}));
        }
        scripts_->send(Value(Value::Map{{"op", Value("inventoryNative")},
                                        {"id", Value(std::int64_t(player))},
                                        {"epoch", Value(double(report.epoch))},
                                        {"sequence", Value(double(report.sequence))},
                                        {"revision", Value(double(report.revision))},
                                        {"changes", Value(std::move(changes))}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::Consume) {
        scripts_->send(Value(Value::Map{{"op", Value("inventoryConsume")},
                                        {"id", Value(std::int64_t(player))},
                                        {"request", Value(double(report.request))},
                                        {"row", Value(report.row)},
                                        {"revision", Value(double(report.revision))}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::PotConsume) {
        const bool known = report.pot != integration::Guid{};
        scripts_->send(Value(
            Value::Map{{"op", Value("inventoryPotConsume")},
                       {"id", Value(std::int64_t(player))},
                       {"request", Value(double(report.request))},
                       {"item", Value(integration::guid_text(report.item))},
                       {"pot", known ? Value(integration::guid_text(report.pot)) : Value()}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::Drop) {
        scripts_->send(Value(Value::Map{{"op", Value("inventoryDrop")},
                                        {"id", Value(std::int64_t(player))},
                                        {"row", Value(report.row)},
                                        {"amount", Value(double(report.request))}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::CustomUse) {
        scripts_->send(Value(Value::Map{{"op", Value("inventoryCustomUse")},
                                        {"id", Value(std::int64_t(player))},
                                        {"row", Value(report.row)},
                                        {"revision", Value(double(report.revision))}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::StashOpen ||
          report.kind == integration::InventoryReport::Kind::StashClose) {
        const bool open = report.kind == integration::InventoryReport::Kind::StashOpen;
        scripts_->send(Value(Value::Map{{"op", Value(open ? "stashOpen" : "stashClose")},
                                        {"id", Value(std::int64_t(player))},
                                        {"stash", Value(double(report.request))}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::StashTake ||
          report.kind == integration::InventoryReport::Kind::StashPut) {
        const bool take = report.kind == integration::InventoryReport::Kind::StashTake;
        scripts_->send(Value(Value::Map{{"op", Value(take ? "stashTake" : "stashPut")},
                                        {"id", Value(std::int64_t(player))},
                                        {"stash", Value(double(report.request))},
                                        {"row", Value(report.row)},
                                        {"amount", Value(double(report.units))}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::HorseGearEquip) {
        scripts_->send(Value(Value::Map{{"op", Value("horseGearEquip")},
                                        {"id", Value(std::int64_t(player))},
                                        {"horse", Value(double(report.request))},
                                        {"row", Value(report.row)},
                                        {"fromBags", Value(report.used)}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::HorseGearRemove) {
        scripts_->send(Value(Value::Map{{"op", Value("horseGearRemove")},
                                        {"id", Value(std::int64_t(player))},
                                        {"horse", Value(double(report.request))},
                                        {"item", Value(integration::guid_text(report.item))}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::Pickup) {
        scripts_->send(Value(Value::Map{{"op", Value("groundItemPickup")},
                                        {"id", Value(std::int64_t(player))},
                                        {"stack", Value(double(report.request))}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::Consumed) {
        scripts_->send(Value(Value::Map{{"op", Value("inventoryConsumed")},
                                        {"id", Value(std::int64_t(player))},
                                        {"request", Value(double(report.request))},
                                        {"used", Value(report.used)}}));
        continue;
      }
      if (report.kind == integration::InventoryReport::Kind::Quickslots) {
        scripts_->send(Value(Value::Map{{"op", Value("inventoryQuickslots")},
                                        {"id", Value(std::int64_t(player))},
                                        {"quickslots", quickslots_value(report.quickslots)}}));
        continue;
      }
      Value::Array rows;
      for (const auto& row : report.rows) {
        rows.push_back(Value(Value::Array{Value(integration::guid_text(row.item)),
                                          Value(double(row.amount)), Value(double(row.quality)),
                                          Value(double(row.health)), Value(double(row.equipped))}));
      }
      scripts_->send(Value(Value::Map{{"op", Value("inventoryCensus")},
                                      {"id", Value(std::int64_t(player))},
                                      {"rows", Value(std::move(rows))}}));
    }
    for (const auto& [player, buff] : server_.take_buff_blocks()) {
      if (!server_.world().slot(player))
        continue;
      scripts_->send(Value(Value::Map{{"op", Value("buffBlocked")},
                                      {"id", Value(std::int64_t(player))},
                                      {"buff", Value(integration::guid_text(buff))}}));
    }
    for (const auto& [player, perk] : server_.take_perk_learnings()) {
      if (!server_.world().slot(player))
        continue;
      scripts_->send(Value(Value::Map{{"op", Value("perkLearning")},
                                      {"id", Value(std::int64_t(player))},
                                      {"perk", Value(integration::perk_id_text(perk))}}));
    }
    for (auto& [player, progression] : server_.take_progressions()) {
      auto stored = progressions_.find(player);
      if (!server_.world().slot(player) ||
          (stored != progressions_.end() && stored->second == progression))
        continue;
      progressions_[player] = std::move(progression);
      if (progression_reported_.count(player))
        report_progression(player);
    }
    for (const auto player : server_.take_knockouts()) {
      scripts_->send(
          Value(Value::Map{{"op", Value("playerDied")}, {"id", Value(std::int64_t(player))}}));
    }
    for (const auto& submission : script_submissions) {
      report_peer_player(submission.player, submission.peer);
      scripts_->send(Value(
          Value::Map{{"op", Value("clientRequest")},
                     {"peer", Value(std::int64_t(submission.peer))},
                     {"player", Value(std::int64_t(submission.player))},
                     {"playerIndex", Value(std::int64_t(player_slots_.slot_of(submission.player)))},
                     {"message", submission.message}}));
    }
    for (const auto& e : submissions) {
      report_player(e.player);
      scripts_->send(Value(Value::Map{
          {"op", Value("chat")}, {"id", Value(std::int64_t(e.player))}, {"text", Value(e.text)}}));
    }
    for (const auto& e : scripts_->take())
      try {
        const auto op = text(e, "op");
        if (voice_op(op, e)) {
          continue;
        }
        // Ops kept out of the chain below, which is at the compiler's nesting limit.
        if (op == "vendorHaggleReply") {
          // The trader's answer to a haggle offer (0.4.1): { id, session, round, answer: 1 accept
          // | 2 counter | 3 mock | 4 refuse, price, mood 0 .. 100 }.
          integration::VendorHaggleReply reply;
          const double session = number(e, "session", 0);
          const double round = number(e, "round", 0);
          const double answer = number(e, "answer", 0);
          const double price = number(e, "price", 0);
          const double mood = number(e, "mood", 0);
          if (!exact_uint32(session) || !exact_uint32(round) || answer < 1 || answer > 4 ||
              std::floor(price) != price ||
              std::fabs(price) > double(integration::kMaxVendorPrice) || !(mood >= 0) ||
              mood > integration::kMaxVendorHaggleMood) {
            std::cerr << "vendor haggle reply refused for session " << session << std::endl;
            continue;
          }
          reply.session = std::uint32_t(session);
          reply.round = std::uint32_t(round);
          reply.answer = integration::VendorHaggleAnswer(std::uint8_t(answer));
          reply.price = std::int64_t(price);
          reply.mood = std::uint8_t(mood);
          server_.send_vendor_haggle_reply(core::EntityId(number(e, "id")), reply);
          continue;
        }
        if (op == "worldItemsRemoved") {
          // World.removeLevelItems: { id (0 everyone), areas: [{ center,
          // radius, item (32 hex digits or '') }] }, the whole list.
          std::vector<integration::RemovedItemArea> areas;
          const auto* list = field(e, "areas");
          if (list && list->tag() == sdk::Tag::Array) {
            for (const Value& entry : list->as_array()) {
              integration::RemovedItemArea area;
              const auto* center = field(entry, "center");
              if (center) {
                read_point(*center, area.center);
              }
              area.radius = float(number(entry, "radius", 0));
              const std::string item = text(entry, "item");
              const bool hex = item.size() == 32 &&
                               std::all_of(item.begin(), item.end(),
                                           [](unsigned char c) { return std::isxdigit(c) != 0; });
              if (hex) {
                for (std::size_t i = 0; i < 16; ++i) {
                  area.item[i] = std::uint8_t(std::stoul(item.substr(i * 2, 2), nullptr, 16));
                }
              }
              areas.push_back(area);
            }
          }
          if (!server_.send_world_items_removed(core::EntityId(number(e, "id")), areas)) {
            std::cerr << "removed item areas not sent" << std::endl;
          }
          continue;
        }
        if (op == "worldLevelEntities") {
          // The level's entities one ready player's game has around a point:
          // { id, request, center, radius, max }; the reply is a worldRaycastReply of hits.
          integration::WorldRayRequest ray;
          ray.request = std::uint32_t(number(e, "request"));
          ray.query = integration::RayQuery::Entities;
          ray.radius = float(number(e, "radius", 0));
          const double most = number(e, "max", integration::kMaxLevelEntities);
          ray.max_hits = most >= 1 && most <= integration::kMaxLevelEntities ? std::uint8_t(most)
                                                                             : std::uint8_t(0);
          const auto* center = field(e, "center");
          if (center) {
            read_point(*center, ray.center);
          }
          if (!center || !server_.send_world_raycast(core::EntityId(number(e, "id")), ray)) {
            std::cerr << "level entities query refused for player " << number(e, "id") << std::endl;
          }
          continue;
        }
        if (op == "horseSpeedScale") {
          // Horse.speedScale: { id, scale } 0.1 to 2, below 0 the game's own.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          const double scale = number(e, "scale", -1);
          if (found != horses_.end() && std::isfinite(scale) && scale <= 2) {
            server_.world().write(found->second, integration::HorseSpeedScale,
                                  float(scale < 0 ? -1 : scale));
          }
          continue;
        }
        if (op == "horseDirt") {
          // Horse.dirt, soil and clean: { id, dirt } 0 to 1.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          const double dirt = number(e, "dirt", -1);
          if (found != horses_.end() && std::isfinite(dirt) && dirt <= 1) {
            server_.world().write(found->second, integration::HorseDirt,
                                  float(dirt < 0 ? -1 : dirt));
          }
          continue;
        }
        if (op == "horseLead") {
          // Horse.follow with rope: { id, leader, at }, 0 for no rope; `at` the joint on the
          // horse it is tied to, "" for the bit.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          const double leader = number(e, "leader", 0);
          const std::string at = text(e, "at");
          if (found != horses_.end() && leader >= 0 && leader <= 4294967295.0 &&
              (at.empty() || integration::lead_rope_joint_fits(at))) {
            server_.world().write(found->second, integration::HorseLeadAt, at);
            server_.world().write(found->second, integration::HorseLead,
                                  std::int64_t(std::floor(leader)));
          }
          continue;
        }
        if (op == "horseRiders") {
          // Horse.allowRider: the players besides its owner allowed to ride it, { id, riders: [
          // network ids] }; every client of theirs offers the plain mount.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          const auto* riders = field(e, "riders");
          if (found == horses_.end() || !riders || riders->tag() != sdk::Tag::Array ||
              riders->as_array().size() > integration::kMaxHorseRiders) {
            continue;
          }
          std::string list;
          for (const Value& rider : riders->as_array()) {
            double id = -1;
            if (rider.tag() == sdk::Tag::Int) {
              id = double(rider.as_int());
            } else if (rider.tag() == sdk::Tag::Double) {
              id = rider.as_double();
            }
            if (!(id > 0) || id > 4294967295.0 || id != std::floor(id)) {
              list.clear();
              break;
            }
            list += (list.empty() ? "" : ",") + std::to_string(std::uint64_t(id));
          }
          server_.world().write(found->second, integration::HorseRiders, list);
          continue;
        }
        if (op == "horsePullDown") {
          // Horse.dragRider: { attacker, rider }; both games ask for it.
          const auto attacker = core::EntityId(number(e, "attacker"));
          const auto rider = core::EntityId(number(e, "rider"));
          server_.send_pull_down_order(attacker, attacker, rider);
          server_.send_pull_down_order(rider, attacker, rider);
          continue;
        }
        if (op == "horseRear") {
          // Horse.buckRider: { id }, the rider.
          server_.send_rear_order(core::EntityId(number(e, "id")));
          continue;
        }
        if (op == "playerKeyBypass") {
          // player.keyBypass: { id, bypass }.
          const auto player = core::EntityId(number(e, "id"));
          if (server_.world().slot(player)) {
            server_.world().write(player, KeyBypass, flag_of(e, "bypass"));
          }
          continue;
        }
        if (op == "playerBrand") {
          // Player.brand: { id, brand }, 0 none, 1 or 2 the game's two marks.
          const auto player = core::EntityId(number(e, "id"));
          const auto brand = std::int64_t(number(e, "brand"));
          if (server_.world().slot(player) && brand >= 0 && brand <= 2) {
            server_.world().write(player, Brand, brand);
          }
          continue;
        }
        if (op == "playerPhysicalProp") {
          // Player.setPhysicalProp: { id, prop }, the field's text or "" for
          // none; anything that does not fit is refused.
          const auto player = core::EntityId(number(e, "id"));
          const std::string prop = text(e, "prop");
          const bool fits = prop.empty() || integration::physical_prop_of(prop).has_value();
          if (server_.world().slot(player) && fits) {
            server_.world().write(player, PhysicalProp, prop);
          }
          continue;
        }
        if (op == "playerLeadRope") {
          // Player.setLeadRope: { id, rope }, the field's text or "" for none.
          const auto player = core::EntityId(number(e, "id"));
          const std::string rope = text(e, "rope");
          const bool fits = rope.empty() || integration::lead_rope_of(rope).has_value();
          if (server_.world().slot(player) && fits) {
            server_.world().write(player, LeadRope, rope);
          }
          continue;
        }
        if (op == "playerPoseAnimation") {
          // Player.setAnimation: { id, animation }, the field's text or "" for none,
          // or the name of one defined by name (an Animations.define longer than the field);
          // anything that does not read back is refused.
          const auto player = core::EntityId(number(e, "id"));
          const std::string animation = text(e, "animation");
          std::string defined;
          bool loops = true;
          const bool fits = animation.empty() ||
                            integration::pose_animation_of(animation).has_value() ||
                            integration::pose_animation_reference(animation, defined, loops);
          if (server_.world().slot(player) && fits) {
            server_.world().write(player, PoseAnimation, animation);
          }
          continue;
        }
        if (op == "stashEmpty") {
          // Whether a scripted container's stock holds nothing: { id, empty }.
          const auto found = stashes_.find(std::uint32_t(number(e, "id")));
          if (found != stashes_.end()) {
            server_.world().write(found->second, integration::StashEmpty, flag_of(e, "empty"));
          }
          continue;
        }
        if (op == "playerAttach") {
          // Attaches one player to another or detaches them: { id, target, right,
          // forward, up, yaw }, target 0 detached.
          integration::PlayerAttach attach;
          attach.player = std::uint32_t(number(e, "id"));
          attach.target = std::uint32_t(number(e, "target"));
          attach.right = float(number(e, "right", 0));
          attach.forward = float(number(e, "forward", 0));
          attach.up = float(number(e, "up", 0));
          attach.yaw = float(number(e, "yaw", 0));
          const auto* follow = field(e, "follow");
          if (follow && follow->tag() == sdk::Tag::Bool && follow->as_bool()) {
            attach.flags = integration::kAttachFollowLook;
          }
          server_.send_player_attach(attach);
          continue;
        }
        if (op == "playerFrozen") {
          // Freezes one player or frees them: { id, frozen }.
          const auto* frozen = field(e, "frozen");
          if (frozen && frozen->tag() == sdk::Tag::Bool) {
            server_.send_frozen(core::EntityId(number(e, "id")), frozen->as_bool());
          }
          continue;
        }
        if (op == "playerLobby") {
          // Holds one player out of the world or lets them in: { id, lobby }.
          const auto* held = field(e, "lobby");
          if (held && held->tag() == sdk::Tag::Bool) {
            server_.send_lobby(core::EntityId(number(e, "id")), held->as_bool());
          }
          continue;
        }
        if (op == "playerNoClipAllowed") {
          // Allows one player NoClip or forbids it: { id, allowed }.
          const auto* allowed = field(e, "allowed");
          if (allowed && allowed->tag() == sdk::Tag::Bool) {
            server_.send_noclip_allowed(core::EntityId(number(e, "id")), allowed->as_bool());
          }
          continue;
        }
        if (op == "worn") {
          // The items a player wears outside their inventory: { id, items: [class
          // as 32 hex digits] }.
          const auto player = core::EntityId(number(e, "id"));
          std::vector<integration::ItemClassId> items;
          if (const auto* list = field(e, "items"); list && list->tag() == sdk::Tag::Array) {
            for (const Value& item : list->as_array()) {
              integration::ItemClassId id{};
              if (item.tag() == sdk::Tag::Str &&
                  integration::parse_item_class_script_text(item.as_str(), id)) {
                items.push_back(id);
              }
            }
          }
          if (!server_.request_worn(player, items)) {
            std::cerr << "worn items refused for player " << player << std::endl;
          }
          continue;
        }
        if (op == "log")
          std::cout << "RESOURCE [" << text(e, "resource") << "] " << text(e, "text") << std::endl;
        else if (op == "resourceReply" || op == "resourceBroadcast") {
          const auto* message = field(e, "message");
          if (!message)
            continue;
          const auto peer =
              op == "resourceBroadcast" ? core::PeerId{0} : core::PeerId(number(e, "peer"));
          server_.send_script(peer, *message);
        } else if (op == "event") {
          // With a player (player.emit) only that player's client gets it; a player
          // without a connection gets nothing.
          const double player = number(e, "player", 0);
          if (player == 0) {
            server_.send_script(0, e);
          } else {
            for (const auto& [key, member] : members_) {
              (void)key;
              if (member.avatar == core::EntityId(player)) {
                server_.send_script(member.peer, e);
              }
            }
          }
        } else if (op == "ready")
          std::cout << "RESOURCE HOST ready" << std::endl;
        else if (op == "revive") {
          const auto player = core::EntityId(number(e, "id"));
          if (!server_.revive(player))
            std::cerr << "revive refused for player " << player << std::endl;
        } else if (op == "knockOut") {
          // Player.knockOut: the game's own unconsciousness with the health the host checked.
          const auto player = core::EntityId(number(e, "id"));
          if (!server_.knock_out(player, float(number(e, "health"))))
            std::cerr << "knock-out refused for player " << player << std::endl;
        } else if (op == "hitVerdict") {
          // playerHit's ruling: the hit player's client takes the blow with this health or not.
          const auto player = core::EntityId(number(e, "id"));
          integration::HitVerdict verdict;
          const double request = number(e, "request", -1);
          verdict.ruling = integration::HitRuling(std::uint8_t(number(e, "ruling", 0)));
          verdict.value = float(number(e, "value", 0));
          // A priced blow priced again: the handlers' modifiers.
          if (verdict.ruling == integration::HitRuling::Priced) {
            verdict.modifiers.attack = float(number(e, "attack", 1));
            verdict.modifiers.defense = float(number(e, "defense", 1));
            verdict.modifiers.block = float(number(e, "block", 1));
            verdict.modifiers.health = float(number(e, "health", 1));
          }
          if (!exact_uint32(request))
            continue;
          verdict.request = std::uint32_t(request);
          if (!server_.send_hit_verdict(player, verdict))
            std::cerr << "hit verdict refused for player " << player << std::endl;
        } else if (op == "hitRules") {
          // Whether some resource handles playerHit: the clients hold blows for a ruling.
          // And whether some resource handles playerHarm: they hold other harm too.
          const sdk::Value* enabled = field(e, "enabled");
          const sdk::Value* harm = field(e, "harm");
          server_.set_hit_rules(enabled && enabled->tag() == sdk::Tag::Bool && enabled->as_bool(),
                                harm && harm->tag() == sdk::Tag::Bool && harm->as_bool());
        } else if (op == "combatCancel") {
          // Player.stopFight: each of the pair's clients drops its lock on the other.
          const auto player = core::EntityId(number(e, "id"));
          const auto opponent = core::EntityId(number(e, "opponent"));
          if (!server_.request_combat_cancel(player, opponent))
            std::cerr << "combat cancel refused for player " << player << std::endl;
        } else if (op == "combatStart") {
          // Player.startFight: each of the pair's clients locks on the other.
          const auto player = core::EntityId(number(e, "id"));
          const auto opponent = core::EntityId(number(e, "opponent"));
          if (!server_.request_combat_start(player, opponent))
            std::cerr << "combat start refused for player " << player << std::endl;
        } else if (op == "worldPickupVerdict") {
          // Whether the player's game takes up the level's item it asked for.
          const auto player = core::EntityId(number(e, "id"));
          const sdk::Value* granted = field(e, "granted");
          if (!server_.send_world_pickup_verdict(
                  player, text(e, "key"),
                  granted && granted->tag() == sdk::Tag::Bool && granted->as_bool()))
            std::cerr << "world pickup verdict refused for player " << player << std::endl;
        } else if (op == "itemNotice") {
          // The game's notice of units the server gave or took.
          integration::ItemNotice notice;
          const double amount = number(e, "amount");
          const sdk::Value* gained = field(e, "gained");
          notice.gained = !gained || gained->tag() != sdk::Tag::Bool || gained->as_bool();
          if (!integration::parse_guid(text(e, "item"), notice.item) || !(amount >= 1) ||
              amount > integration::kMaxItemNoticeAmount) {
            continue;
          }
          notice.amount = std::uint32_t(amount);
          server_.send_item_notice(core::EntityId(number(e, "id")), notice);
        } else if (op == "pickpocketPockets") {
          // What a victim carries, for the thief's loot screen: { id, victim,
          // rows: [[class GUID, amount, quality, health]] }.
          integration::PickpocketPockets pockets;
          pockets.victim = std::uint32_t(number(e, "victim", 0));
          const sdk::Value* rows = field(e, "rows");
          bool valid = rows && rows->tag() == sdk::Tag::Array &&
                       rows->as_array().size() <= integration::kMaxPickpocketRows;
          for (std::size_t i = 0; valid && i < rows->as_array().size(); ++i) {
            const sdk::Value& cells = rows->as_array()[i];
            if (cells.tag() != sdk::Tag::Array || cells.as_array().size() != 4 ||
                cells.as_array()[0].tag() != sdk::Tag::Str) {
              valid = false;
              break;
            }
            const auto& c = cells.as_array();
            auto num = [](const Value& v) {
              return v.tag() == sdk::Tag::Int      ? double(v.as_int())
                     : v.tag() == sdk::Tag::Double ? v.as_double()
                                                   : -1;
            };
            integration::InventoryRowState row;
            const double amount = num(c[1]);
            const double quality = num(c[2]);
            const double health = num(c[3]);
            if (!integration::parse_guid(c[0].as_str(), row.item) || !(amount >= 1) ||
                amount > 4294967295.0 || !(quality >= 0) || quality > 255 || !(health >= 0)) {
              valid = false;
              break;
            }
            row.amount = std::uint32_t(amount);
            row.quality = std::uint8_t(quality);
            row.health = float(health);
            pockets.rows.push_back(row);
          }
          if (valid)
            server_.send_pickpocket_pockets(core::EntityId(number(e, "id")), pockets);
        } else if (op == "worldTime") {
          // The world clock for one ready player: { id, day, hour, timeScale }.
          integration::WorldTime time;
          const double day = number(e, "day", -1);
          if (!(day >= 0) || day > integration::kMaxWorldDay) {
            std::cerr << "world time refused: day " << day << std::endl;
            continue;
          }
          time.day = std::uint32_t(day);
          time.hour = float(number(e, "hour", 0));
          time.timeScale = float(number(e, "timeScale", 0));
          if (!server_.send_world_time(core::EntityId(number(e, "id")), time))
            std::cerr << "world time refused for player " << number(e, "id") << std::endl;
        } else if (op == "gateState") {
          // A gate's state for one ready player: { id, gate, state, elapsedMs,
          // barOut, barElapsedMs }.
          integration::GateState gate;
          const double index = number(e, "gate", -1);
          const double state = number(e, "state", -1);
          if (!(index >= 0) || index >= double(integration::kGateCount) || !(state >= 0) ||
              state >= double(integration::gate_state_count(std::uint32_t(index)))) {
            std::cerr << "gate refused: " << index << " state " << state << std::endl;
            continue;
          }
          gate.gate = std::uint32_t(index);
          gate.state = std::uint8_t(state);
          gate.bar_out = flag_of(e, "barOut");
          const double bar_elapsed = number(e, "barElapsedMs", 0);
          gate.bar_elapsed_ms =
              bar_elapsed > 0 ? std::uint32_t(std::min(bar_elapsed, 600000.0)) : 0;
          const double elapsed = number(e, "elapsedMs", 0);
          gate.elapsed_ms = elapsed > 0 ? std::uint32_t(std::min(elapsed, 600000.0)) : 0;
          if (!server_.send_gate(core::EntityId(number(e, "id")), gate))
            std::cerr << "gate refused for player " << number(e, "id") << std::endl;
        } else if (op == "worldWeather") {
          // The weather for one ready player: { id, preset, previous, seconds,
          // remaining, rainStrength, rainLevel, wind: { x, y, z },
          // snow: { amount, frost, freezing, flakes } }.
          integration::WorldWeather weather;
          weather.preset = text(e, "preset");
          weather.previous = text(e, "previous");
          weather.seconds = float(number(e, "seconds", 0));
          weather.remaining = float(number(e, "remaining", 0));
          weather.rainStrength = float(number(e, "rainStrength", 0));
          weather.rainLevel = float(number(e, "rainLevel", 0));
          if (const sdk::Value* wind = field(e, "wind")) {
            weather.windX = float(number(*wind, "x", 0));
            weather.windY = float(number(*wind, "y", 0));
            weather.windZ = float(number(*wind, "z", 0));
          }
          if (const sdk::Value* snow = field(e, "snow")) {
            weather.snowAmount = float(number(*snow, "amount", 0));
            weather.snowFrost = float(number(*snow, "frost", 0));
            weather.snowFreezing = float(number(*snow, "freezing", 0));
            const double flakes = number(*snow, "flakes", 0);
            weather.snowFlakes =
                flakes > 0 && flakes <= integration::kMaxSnowFlakes ? std::uint32_t(flakes) : 0;
          }
          if (!server_.send_world_weather(core::EntityId(number(e, "id")), weather))
            std::cerr << "world weather refused for player " << number(e, "id") << std::endl;
        } else if (op == "markerShow" || op == "markerHide" || op == "blipShow" ||
                   op == "blipHide") {
          // A world marker or a compass and map blip for one ready player.
          if (!send_marker_op(op, e))
            std::cerr << "marker op refused: " << op << " for player " << number(e, "id")
                      << std::endl;
        } else if (op == "questShow" || op == "questHide" || op == "questTrack") {
          // A quest in one ready player's journal, or whether they follow it.
          if (!send_quest_op(op, e))
            std::cerr << "quest op refused: " << op << " for player " << number(e, "id")
                      << std::endl;
        } else if (op == "graveSet") {
          // A grave's state for one ready player or all (0): { to, grave, dug }.
          integration::Grave grave;
          grave.kind = integration::GraveKind::Set;
          grave.dug = flag_of(e, "dug");
          if (!integration::parse_grave_id(text(e, "grave"), grave.position)) {
            std::cerr << "grave set refused: bad grave '" << text(e, "grave") << "'" << std::endl;
          } else {
            server_.send_grave_set(core::EntityId(number(e, "to")), grave);
          }
        } else if (op == "doorSet") {
          // The state of one door for one ready player: { id, guid, open, side,
          // locked }.
          integration::DoorState state;
          if (!integration::parse_door_guid(text(e, "guid"), state.guid)) {
            std::cerr << "door set refused: bad guid '" << text(e, "guid") << "'" << std::endl;
          } else {
            state.open = flag_of(e, "open");
            state.side = flag_of(e, "side");
            state.locked = flag_of(e, "locked");
            // The script's lock-picking difficulty, below 0 the level's own.
            state.difficulty = float(number(e, "difficulty", -1));
            // The key a script set, "" for the level's own.
            state.key = text(e, "key");
            // The lock kind a script set (esLockTypes), "" for the door's own.
            state.lock_types = text(e, "lockTypes");
            // A script's lock, which the game may not open for its NPCs either.
            state.script_lock = flag_of(e, "scriptLock");
            if (!server_.send_door_set(core::EntityId(number(e, "id")), state))
              std::cerr << "door set refused for player " << number(e, "id") << std::endl;
          }
        } else if (op == "smartObjectSpawn" || op == "smartObjectDestroy") {
          // A smart object a script placed, or took down
          // (smart_objects/placed_objects.cjs): to every ready player whose world sees it (no
          // `to`) or to the one in `to`.
          integration::SmartObjectOrder order;
          order.id = std::uint32_t(number(e, "id"));
          order.remove = op == "smartObjectDestroy";
          bool valid = true;
          if (!order.remove) {
            std::array<float, 3> position{};
            std::array<float, 4> rotation{};
            valid = integration::parse_template_guid(text(e, "template"), order.template_guid) &&
                    floats_of(e, "position", position) && floats_of(e, "rotation", rotation);
            std::copy(position.begin(), position.end(), order.position);
            std::copy(rotation.begin(), rotation.end(), order.rotation);
            // The entities with a brush among their parents, placed where they stand.
            const Value* corrections = field(e, "corrections");
            if (corrections && corrections->tag() == sdk::Tag::Array) {
              for (const Value& item : corrections->as_array()) {
                integration::SmartObjectCorrection correction;
                std::array<float, 3> at{};
                std::array<float, 4> turn{};
                correction.place = std::uint16_t(number(item, "place"));
                valid = valid && floats_of(item, "at", at) && floats_of(item, "turn", turn);
                std::copy(at.begin(), at.end(), correction.at);
                std::copy(turn.begin(), turn.end(), correction.turn);
                order.corrections.push_back(correction);
              }
            }
          }
          const auto world = std::uint32_t(number(e, "dimension", 0xffffffff));
          if (!valid || !server_.send_smart_object(core::EntityId(number(e, "to")), order, world))
            std::cerr << "smart object " << order.id << " not sent" << std::endl;
        } else if (op == "doorSpawn" || op == "doorDespawn") {
          // A door a script hung, or took down (doors/doors.cjs): to every ready
          // player whose world sees it (no `to`) or to the one in `to`.
          integration::DoorSpawn door;
          door.remove = op == "doorDespawn";
          if (!integration::parse_door_guid(text(e, "guid"), door.guid)) {
            std::cerr << "door spawn refused: bad guid '" << text(e, "guid") << "'" << std::endl;
          } else {
            if (!door.remove) {
              std::array<float, 3> position{};
              std::array<float, 4> rotation{};
              floats_of(e, "position", position);
              floats_of(e, "rotation", rotation);
              std::copy(position.begin(), position.end(), door.position);
              std::copy(rotation.begin(), rotation.end(), door.rotation);
              door.model = text(e, "model");
              door.anim_set = text(e, "animSet");
              door.helpers = text(e, "helpers");
              door.smart_object_type = text(e, "smartObjectType");
              door.rigid = flag_of(e, "rigid");
              door.pushable = flag_of(e, "pushable");
              door.locked = flag_of(e, "locked");
              door.lock_types = text(e, "lockTypes");
              door.canBePicked = flag_of(e, "canBePicked");
              door.difficulty = float(number(e, "difficulty", -1));
              door.key = text(e, "key");
            }
            const auto world = std::uint32_t(number(e, "dimension", 0xffffffff));
            if (!server_.send_door_spawn(core::EntityId(number(e, "to")), door, world))
              std::cerr << "door spawn " << text(e, "guid") << " not sent" << std::endl;
          }
        } else if (op == "doorNearest") {
          // Asks one ready player's game for the door nearest in sight: { id,
          // request }.
          if (!server_.send_door_nearest(core::EntityId(number(e, "id")),
                                         std::uint32_t(number(e, "request", 0))))
            std::cerr << "door nearest refused for player " << number(e, "id") << std::endl;
        } else if (op == "propSpawn" || op == "propUpdate" || op == "propDestroy" ||
                   op == "propMove" || op == "propSimulate" || op == "propPush") {
          // Props: sent to every ready player (no `to`) or to the one in `to`. A rigid
          // one's simulating client and its pushes go to one player.
          const auto kind = op == "propSpawn"      ? integration::PropKind::Spawn
                            : op == "propUpdate"   ? integration::PropKind::Update
                            : op == "propMove"     ? integration::PropKind::Move
                            : op == "propSimulate" ? integration::PropKind::Simulate
                            : op == "propPush"     ? integration::PropKind::Push
                                                   : integration::PropKind::Destroy;
          server_.send_prop(core::EntityId(number(e, "to")), prop_order_of(e, kind));
        } else if (op == "vfxSpawn" || op == "vfxUpdate" || op == "vfxDestroy") {
          // Placed effects: sent to every ready player (no `to`) or to the one in `to`.
          const auto kind = op == "vfxSpawn"    ? integration::VfxKind::Spawn
                            : op == "vfxUpdate" ? integration::VfxKind::Update
                                                : integration::VfxKind::Destroy;
          integration::VfxOrder order = vfx_order_of(e, kind);
          order.id = std::uint32_t(number(e, "id"));
          order.position = floats3(e, "position");
          // A spawn on a player's or NPC's body.
          const double body = number(e, "body", 0);
          order.body = body >= 1 && body <= 4294967295.0 ? std::uint32_t(body) : 0;
          order.joint = order.body != 0 ? text(e, "joint") : std::string();
          server_.send_vfx(core::EntityId(number(e, "to")), order);
        } else if (op == "recipeTable") {
          // A recipe a resource registered: its table patch, to every ready player
          // (no `to`) or to the one in `to`.
          integration::RecipeTable table;
          table.file = text(e, "file");
          table.patch = text(e, "patch");
          // Its own icon, when it has one: the item icon's name and its .dds.
          table.icon_name = text(e, "iconName");
          if (const Value* icon = field(e, "icon"); icon && icon->tag() == sdk::Tag::Bytes)
            table.icon = icon->as_bytes();
          if (!server_.send_recipe_table(core::EntityId(number(e, "to")), table))
            std::cerr << "recipe table " << table.file << " not sent" << std::endl;
        } else if (op == "recipePicture") {
          // One of an alchemy recipe's pictures in the recipe book, after its table.
          integration::RecipePicture picture;
          picture.which = text(e, "which") == "recipe" ? integration::RecipePictureKind::Recipe
                                                       : integration::RecipePictureKind::Product;
          picture.recipe_id = std::uint32_t(number(e, "id"));
          picture.from = std::uint32_t(number(e, "from"));
          if (const Value* dds = field(e, "dds"); dds && dds->tag() == sdk::Tag::Bytes)
            picture.dds = dds->as_bytes();
          if (!server_.send_recipe_picture(core::EntityId(number(e, "to")), picture))
            std::cerr << "recipe picture of " << picture.recipe_id << " not sent" << std::endl;
        } else if (op == "lightSpawn" || op == "lightUpdate" || op == "lightDestroy") {
          // Point lights (lights/lights.cjs): to every ready player (no `to`) or to
          // the one in `to`.
          integration::LightOrder order;
          order.kind = op == "lightSpawn"    ? integration::LightKind::Spawn
                       : op == "lightUpdate" ? integration::LightKind::Update
                                             : integration::LightKind::Destroy;
          order.id = std::uint32_t(number(e, "id"));
          if (order.kind != integration::LightKind::Destroy) {
            order.position = floats3(e, "position");
            order.color = floats3(e, "color");
            order.intensity = float(number(e, "intensity", 1));
            order.radius = float(number(e, "radius", 5));
            order.cast_shadows = flag_of(e, "castShadows");
            // On a player's or NPC's body, or one of its joints.
            const double body = number(e, "body");
            order.body = exact_uint32(body) ? std::uint32_t(body) : 0;
            order.joint = text(e, "joint");
          }
          server_.send_light(core::EntityId(number(e, "to")), order);
        } else if (op == "vfxBurst" || op == "vfxBurstOn") {
          // A burst is played once by the players named in `players`.
          const bool on_entity = op == "vfxBurstOn";
          integration::VfxOrder order = vfx_order_of(
              e, on_entity ? integration::VfxKind::BurstOn : integration::VfxKind::Burst);
          order.id = on_entity ? std::uint32_t(number(e, "entity")) : 0;
          order.position = floats3(e, on_entity ? "offset" : "position");
          order.joint = on_entity ? text(e, "joint") : std::string();
          const sdk::Value* players = field(e, "players");
          if (players && players->tag() == sdk::Tag::Array) {
            for (const sdk::Value& player : players->as_array()) {
              if (player.tag() == sdk::Tag::Int || player.tag() == sdk::Tag::Double) {
                const double id =
                    player.tag() == sdk::Tag::Int ? double(player.as_int()) : player.as_double();
                if (exact_uint32(id) && id > 0)
                  server_.send_vfx(core::EntityId(id), order);
              }
            }
          }
        } else if (cart_kind_of(op)) {
          // Carts: to every ready player (no `to`) or to the one in `to`.
          integration::CartOrder order;
          order.kind = *cart_kind_of(op);
          order.id = std::uint32_t(number(e, "id"));
          // A load's name travels in the order's text, as a spawn's blueprint does.
          order.blueprint =
              text(e, order.kind == integration::CartKind::Load ? "load" : "blueprint");
          order.position = floats3(e, "position");
          order.rotation = quaternion4(e, "rotation");
          order.dimension = std::uint32_t(number(e, "dimension"));
          order.max_speed = float(number(e, "maxSpeed", 0));
          order.player = std::uint32_t(
              number(e, order.kind == integration::CartKind::Pose ? "driver" : "player"));
          order.seat =
              integration::cart_seat_of(text(e, "seat")).value_or(integration::CartSeat::Driver);
          order.pace =
              integration::cart_pace_of(text(e, "pace")).value_or(integration::CartPace::Stand);
          server_.send_cart(core::EntityId(number(e, "to")), order);
        } else if (op == "siegeSpawn" || op == "siegeUpdate" || op == "siegeDestroy") {
          // Siege engines: sent to every ready player (no `to`) or to the one in `to`.
          integration::SiegeEngineOrder order;
          order.kind = op == "siegeSpawn"    ? integration::SiegeOrderKind::Spawn
                       : op == "siegeUpdate" ? integration::SiegeOrderKind::Update
                                             : integration::SiegeOrderKind::Destroy;
          order.id = siege_id_of(e, "id");
          bool valid = order.id != 0;
          if (valid && order.kind == integration::SiegeOrderKind::Spawn) {
            order.engine = text(e, "kind") == "cannon" ? integration::SiegeEngineKind::Cannon
                                                       : integration::SiegeEngineKind::Trebuchet;
            order.dimension =
                std::uint32_t(number(e, "dimension", double(replication::kGlobalWorld)));
            valid = floats_of(e, "position", order.position);
          }
          if (valid && order.kind != integration::SiegeOrderKind::Destroy) {
            valid = siege_state_of(e, order.state);
          }
          const auto operator_id = core::EntityId(number(e, "operator", 0));
          if (!valid) {
            std::cerr << op << " refused: malformed engine " << number(e, "id", 0) << std::endl;
          } else {
            server_.send_siege_engine(core::EntityId(number(e, "to", 0)), order, operator_id);
          }
        } else if (op == "siegeShot") {
          // A shot let go: drawn by every ready player in its world, reported by one.
          integration::SiegeShot shot;
          shot.engine = siege_id_of(e, "id");
          shot.shot = siege_id_of(e, "shot");
          shot.flight = float(number(e, "flight", 0));
          shot.apex = float(number(e, "apex", 0));
          const bool valid = shot.engine != 0 && shot.shot != 0 &&
                             floats_of(e, "from", shot.from) && floats_of(e, "target", shot.target);
          const auto world =
              std::uint32_t(number(e, "dimension", double(replication::kGlobalWorld)));
          if (!valid ||
              !server_.send_siege_shot(shot, world, core::EntityId(number(e, "reporter", 0)))) {
            std::cerr << "siege shot " << number(e, "shot", 0) << " reached nobody" << std::endl;
          }
        } else if (op == "ghostCreate" || op == "ghostPose" || op == "ghostTint" ||
                   op == "ghostHidden" || op == "ghostDestroy") {
          // A player's prop preview: `id` is the player it is shown to.
          const auto kind = op == "ghostCreate"   ? integration::GhostKind::Create
                            : op == "ghostPose"   ? integration::GhostKind::Pose
                            : op == "ghostTint"   ? integration::GhostKind::Tint
                            : op == "ghostHidden" ? integration::GhostKind::Hidden
                                                  : integration::GhostKind::Destroy;
          if (!server_.send_ghost(core::EntityId(number(e, "id")), ghost_order_of(e, kind)))
            std::cerr << op << " refused for player " << number(e, "id") << std::endl;
        } else if (op == "placerBegin" || op == "placerVeto" || op == "placerEnd") {
          // A player's prop placement: `id` is the player who places.
          integration::PlacerOrder order;
          if (op == "placerBegin") {
            order = placer_begin_of(e);
          } else {
            order.kind =
                op == "placerVeto" ? integration::PlacerKind::Veto : integration::PlacerKind::End;
            order.session = std::uint32_t(number(e, "session"));
            order.vetoed = flag_of(e, "vetoed");
          }
          if (!server_.send_placer(core::EntityId(number(e, "id")), order))
            std::cerr << op << " refused for player " << number(e, "id") << std::endl;
        } else if (op == "gizmoDraw" || op == "gizmoClear") {
          // Gizmos: drawn for the players in `players`, or for every ready player.
          integration::GizmoOrder order;
          if (op == "gizmoDraw") {
            order = gizmo_draw_of(e);
          } else {
            order.gizmo = std::uint32_t(number(e, "gizmo"));
            order.kind =
                order.gizmo == 0 ? integration::GizmoKind::ClearAll : integration::GizmoKind::Clear;
          }
          for (const core::EntityId player : gizmo_players_of(e)) {
            server_.send_gizmo(player, order);
          }
        } else if (op == "siegeLadderSpawn" || op == "siegeLadderPose" ||
                   op == "siegeLadderDestroy") {
          // Siege ladders: sent to every ready player (no `to`) or to the one in `to`.
          integration::SiegeLadderOrder order;
          order.kind = op == "siegeLadderSpawn"  ? integration::LadderMessageKind::Spawn
                       : op == "siegeLadderPose" ? integration::LadderMessageKind::Pose
                                                 : integration::LadderMessageKind::Destroy;
          order.id = siege_id_of(e, "id");
          bool valid = order.id != 0;
          if (valid && order.kind == integration::LadderMessageKind::Spawn) {
            order.height = float(number(e, "height", 0));
            order.lean = float(number(e, "lean", 0));
            order.dimension =
                std::uint32_t(number(e, "dimension", double(replication::kGlobalWorld)));
            valid = ladder_kind_of(e, order.ladder) && floats_of(e, "position", order.position) &&
                    floats_of(e, "rotation", order.rotation);
          }
          if (valid && order.kind != integration::LadderMessageKind::Destroy) {
            order.usable = flag_of(e, "usable");
            valid = ladder_pose_of(e, order.pose);
          }
          const auto actor = core::EntityId(number(e, "actor", 0));
          if (!valid) {
            std::cerr << op << " refused: malformed ladder " << number(e, "id", 0) << std::endl;
          } else {
            server_.send_siege_ladder(core::EntityId(number(e, "to", 0)), order, actor);
          }
        } else if (op == "siegeLadderRefused") {
          // A push or raise the server did not allow; `id` is the player whose game asked.
          integration::SiegeLadderOrder order;
          order.kind = integration::LadderMessageKind::Refused;
          const auto player = core::EntityId(number(e, "id", 0));
          if (player != 0) {
            server_.send_siege_ladder(player, order, 0);
          }
        } else if (op == "stonePileSpawn" || op == "stonePileUpdate" || op == "stonePileDestroy") {
          // Stone piles: sent to every ready player (no `to`) or to the one in `to`.
          integration::StonePileOrder order;
          order.kind = op == "stonePileSpawn"    ? integration::StonePileMessageKind::Spawn
                       : op == "stonePileUpdate" ? integration::StonePileMessageKind::Update
                                                 : integration::StonePileMessageKind::Destroy;
          order.id = siege_id_of(e, "id");
          bool valid = order.id != 0;
          if (valid && order.kind == integration::StonePileMessageKind::Spawn) {
            order.dimension =
                std::uint32_t(number(e, "dimension", double(replication::kGlobalWorld)));
            valid = floats_of(e, "position", order.position) &&
                    floats_of(e, "rotation", order.rotation);
          }
          if (valid && order.kind != integration::StonePileMessageKind::Destroy) {
            order.usable = flag_of(e, "usable");
            valid = pile_stones_of(e, order.stones);
          }
          if (!valid) {
            std::cerr << op << " refused: malformed pile " << number(e, "id", 0) << std::endl;
          } else {
            server_.send_stone_pile(core::EntityId(number(e, "to", 0)), order);
          }
        } else if (op == "grimeOrder") {
          // Clean or soil one player: { id, kind: clean | soil, body, clothes, dirt,
          // blood } (dirt and blood 0 .. 255).
          integration::GrimeOrder order;
          const std::string kind = text(e, "kind");
          if (kind == "clean")
            order.kind = integration::GrimeOrder::Kind::Clean;
          else if (kind == "soil")
            order.kind = integration::GrimeOrder::Kind::Soil;
          else
            continue;
          const sdk::Value* body = field(e, "body");
          const sdk::Value* clothes = field(e, "clothes");
          order.body = !body || body->tag() != sdk::Tag::Bool || body->as_bool();
          order.clothes = !clothes || clothes->tag() != sdk::Tag::Bool || clothes->as_bool();
          order.dirt = std::uint8_t(std::clamp(number(e, "dirt", 0), 0.0, 255.0));
          order.blood = std::uint8_t(std::clamp(number(e, "blood", 0), 0.0, 255.0));
          server_.send_grime_order(core::EntityId(number(e, "id")), order);
        } else if (op == "bodyCarry") {
          // A carry of a player's body for one client: { id, kind: carry | drop |
          // putDown | refuse, carrier, carried }.
          integration::BodyCarry carry;
          const std::string kind = text(e, "kind");
          if (kind == "carry")
            carry.kind = integration::BodyCarry::Kind::Carry;
          else if (kind == "drop")
            carry.kind = integration::BodyCarry::Kind::Drop;
          else if (kind == "putDown")
            carry.kind = integration::BodyCarry::Kind::PutDown;
          else if (kind == "refuse")
            carry.kind = integration::BodyCarry::Kind::Refuse;
          else
            continue;
          carry.carrier = std::uint32_t(number(e, "carrier", 0));
          carry.carried = std::uint32_t(number(e, "carried", 0));
          server_.send_body_carry(core::EntityId(number(e, "id")), carry);
        } else if (op == "animation") {
          // Player.playAnimation and stopAnimation for the player's own client: the
          // host checked the arguments; the codec checks them again.
          const auto player = core::EntityId(number(e, "id"));
          integration::AnimationOrder order;
          if (!animation_order_of(e, order) || !server_.send_animation_order(player, order)) {
            std::cerr << "animation order refused for player " << player << std::endl;
          }
        } else if (op == "freecam") {
          // Whether one player may fly the free camera: { id, allowed }.
          const sdk::Value* allowed = field(e, "allowed");
          server_.send_freecam(core::EntityId(number(e, "id")),
                               !allowed || allowed->tag() != sdk::Tag::Bool || allowed->as_bool());
        } else if (op == "pickpocketRule") {
          // A pickpocketing rule for one player's client: { id, kind: enabled |
          // answer | noticed, flag, player, notice (the victim's text) }.
          integration::PickpocketRule rule;
          const std::string kind = text(e, "kind");
          if (kind == "enabled")
            rule.kind = integration::PickpocketRule::Kind::Enabled;
          else if (kind == "answer")
            rule.kind = integration::PickpocketRule::Kind::Answer;
          else if (kind == "noticed")
            rule.kind = integration::PickpocketRule::Kind::Noticed;
          else if (kind == "stolenMarks")
            rule.kind = integration::PickpocketRule::Kind::StolenMarks;
          else
            continue;
          const sdk::Value* flag = field(e, "flag");
          rule.flag = !flag || flag->tag() != sdk::Tag::Bool || flag->as_bool();
          rule.player = std::uint32_t(number(e, "player", 0));
          rule.text = text(e, "notice").substr(0, integration::kMaxPickpocketNoticeBytes);
          server_.send_pickpocket_rule(core::EntityId(number(e, "id")), rule);
        } else if (op == "worldTaken") {
          // The level's items taken up, for one player or everyone (0).
          const auto player = core::EntityId(number(e, "id"));
          integration::WorldTaken taken;
          const sdk::Value* full = field(e, "full");
          taken.full = full && full->tag() == sdk::Tag::Bool && full->as_bool();
          const sdk::Value* keys = field(e, "keys");
          if (keys && keys->tag() == sdk::Tag::Array) {
            for (const sdk::Value& key : keys->as_array()) {
              if (key.tag() == sdk::Tag::Str)
                taken.keys.push_back(key.as_str());
            }
          }
          if (!server_.send_world_taken(player, taken))
            std::cerr << "world taken list not sent for player " << player << std::endl;
        } else if (op == "serverStop") {
          // KCDS-Admin or a scheduled restart: the main loop ends after this tick.
          const sdk::Value* restart = field(e, "restart");
          StopRequest request;
          request.reason = transport::sanitize_refusal_text(text(e, "reason"));
          request.restart = restart && restart->tag() == sdk::Tag::Bool && restart->as_bool();
          stop_request_ = std::move(request);
        } else if (op == "worldItemsEnabled") {
          // World.setLevelItemsEnabled: { id (0 everyone), enabled }.
          const sdk::Value* enabled = field(e, "enabled");
          if (enabled && enabled->tag() == sdk::Tag::Bool) {
            server_.send_world_items_enabled(core::EntityId(number(e, "id")), enabled->as_bool());
          }
        } else if (op == "carryOrder") {
          // Player.holdItem and dropHeldItem: the player's client carries or puts down.
          const auto player = core::EntityId(number(e, "id"));
          integration::CarryOrder order;
          order.kind = integration::CarryOrderKind(std::uint8_t(number(e, "kind")));
          bool known = order.kind != integration::CarryOrderKind::Carry ||
                       integration::parse_guid(text(e, "item"), order.item);
          if (order.kind == integration::CarryOrderKind::CarryStack) {
            // A stack by its id in scripts, sent as its network id.
            const double script = number(e, "stack");
            auto found = exact_uint32(script) ? ground_items_.find(std::uint32_t(script))
                                              : ground_items_.end();
            known = found != ground_items_.end();
            order.stack = known ? std::uint32_t(found->second) : 0;
          }
          if (!known || !server_.request_carry(player, order))
            std::cerr << "carry order refused for player " << player << std::endl;
        } else if (op == "knockoutVision") {
          const auto player = core::EntityId(number(e, "id"));
          const auto percent = number(e, "percent");
          if (percent < 0 || percent > integration::kKnockoutVisionMax ||
              !server_.set_knockout_vision(player, std::uint8_t(percent)))
            std::cerr << "knockout vision refused for player " << player << std::endl;
        } else if (op == "combatMarker") {
          // Whether one player sees the game's combat marker: { id, shown }.
          const sdk::Value* shown = field(e, "shown");
          server_.send_combat_marker(core::EntityId(number(e, "id")),
                                     !shown || shown->tag() != sdk::Tag::Bool || shown->as_bool());
        } else if (op == "kick") {
          // A resource kicks a player (Player.kick) or lets one go
          // (Player.disconnect): { id, reason, kind? }, kind 'leave' or 'quit' for
          // the latter. Its client hears why at once and the player is dropped half a second later.
          const auto player = core::EntityId(number(e, "id"));
          const std::string reason = text(e, "reason");
          const std::string kind = text(e, "kind");
          const auto end_kind = kind == "quit"    ? integration::SessionEndKind::Quit
                                : kind == "leave" ? integration::SessionEndKind::Left
                                : kind == "ban"   ? integration::SessionEndKind::Banned
                                                  : integration::SessionEndKind::Kicked;
          if (kicks_.count(player)) {
            continue;
          }
          if (server_.send_session_end(player, {end_kind, reason})) {
            kicks_[player] = clock_seconds() + kKickDropDelaySeconds;
          } else {
            std::cerr << "kick refused for player " << player << std::endl;
          }
        } else if (op == "playerNameplate") {
          // Player.setNameplateVisible, setNameplateHealthVisible, setNameplateText and
          // setNameplateColor: { id, field }, the whole tag as nameplates.hpp keeps
          // it.
          const auto player = core::EntityId(number(e, "id"));
          std::string tag = text(e, "field");
          integration::NameplateState state;
          if (server_.world().slot(player) && integration::parse_nameplate_field(tag, state)) {
            server_.world().write(player, Nameplate, std::move(tag));
          }
        } else if (op == "restrictedAreaMarker") {
          // Whether one player sees the game's restricted-area marker: { id, shown }.
          const sdk::Value* shown = field(e, "shown");
          server_.send_restricted_area_marker(
              core::EntityId(number(e, "id")),
              !shown || shown->tag() != sdk::Tag::Bool || shown->as_bool());
        } else if (op == "thirdPerson") {
          // Which third-person mode one player's camera follows: { id, mode }, the
          // mode being 0 allowed, 1 disabled, 2 forced.
          const double mode = number(e, "mode", -1);
          if (mode >= 0 && mode <= integration::kThirdPersonForced) {
            server_.send_third_person(core::EntityId(number(e, "id")), std::uint8_t(mode));
          }
        } else if (op == "worldRaycast") {
          // A ray for one ready player's game to trace: { id, request, query: 0 the
          // first hit, 1 every hit, start, end, mode: 0 cover, 1 anything, 2 ground, 3 terrain,
          // max }.
          integration::WorldRayRequest ray;
          ray.request = std::uint32_t(number(e, "request"));
          const double query = number(e, "query", 0);
          const double mode = number(e, "mode", 0);
          const double max_hits = number(e, "max", 1);
          const auto* from = field(e, "start");
          const auto* to = field(e, "end");
          const bool known = (query == 0 || query == 1) && mode >= 0 &&
                             mode < integration::kRayModeCount && max_hits >= 1 &&
                             max_hits <= integration::kMaxRayHits;
          if (from && to && known) {
            ray.query = integration::RayQuery(std::uint8_t(query));
            ray.mode = integration::RayMode(std::uint8_t(mode));
            ray.max_hits = std::uint8_t(max_hits);
            read_point(*from, ray.from);
            read_point(*to, ray.to);
          }
          if (!from || !to || !known ||
              !server_.send_world_raycast(core::EntityId(number(e, "id")), ray))
            std::cerr << "world raycast refused for player " << number(e, "id") << std::endl;
        } else if (op == "worldBodiesInRadius") {
          // The replicated bodies one ready player's game shows around a point:
          // { id, request, center, radius }.
          integration::WorldRayRequest ray;
          ray.request = std::uint32_t(number(e, "request"));
          ray.query = integration::RayQuery::Bodies;
          ray.radius = float(number(e, "radius", 0));
          const auto* center = field(e, "center");
          if (center) {
            read_point(*center, ray.center);
          }
          if (!center || !server_.send_world_raycast(core::EntityId(number(e, "id")), ray))
            std::cerr << "world bodies query refused for player " << number(e, "id") << std::endl;
        } else if (op == "cameraShot") {
          // A camera shot for one ready player: { id, position, rotation | lookAt and
          // roll, fov, seconds }.
          integration::CameraShot shot;
          const auto* position = field(e, "position");
          const auto* look_at = field(e, "lookAt");
          const auto* rotation = field(e, "rotation");
          if (position) {
            read_point(*position, shot.position);
          }
          if (look_at) {
            shot.aim = integration::CameraAim::LookAt;
            read_point(*look_at, shot.target);
            shot.roll = float(number(e, "roll", 0));
          } else if (rotation) {
            shot.aim = integration::CameraAim::Rotation;
            shot.rotation[0] = float(number(*rotation, "x"));
            shot.rotation[1] = float(number(*rotation, "y"));
            shot.rotation[2] = float(number(*rotation, "z"));
            shot.rotation[3] = float(number(*rotation, "w", 1));
          }
          shot.fov = float(number(e, "fov", 0));
          shot.seconds = float(number(e, "seconds", 0));
          shot.ease = flag(e, "ease", true);
          // look: how far the mouse turns it, 0 for a still shot.
          const auto look = [&](const char* key, double highest) {
            const double value = number(e, key, 0);
            return std::uint8_t(
                std::clamp(std::floor(std::isfinite(value) ? value : 0), 0.0, highest));
          };
          shot.look_yaw = look("lookYaw", 180);
          shot.look_up = look("lookUp", 89);
          shot.look_down = look("lookDown", 89);
          if (!position || !server_.send_camera_shot(core::EntityId(number(e, "id")), shot))
            std::cerr << "camera shot refused for player " << number(e, "id") << std::endl;
        } else if (op == "cameraClear") {
          // Gives one ready player their own view back: { id }.
          server_.send_camera_clear(core::EntityId(number(e, "id")));
        } else if (op == "interactions") {
          // One player's whole list of world interactions: { id, list: [{ id,
          // entity, body, position, radius, key, hold, holdSeconds, targetRadius, targetHeight,
          // text }] }; body is a player's or NPC's network id, 0 for none.
          const auto* list = field(e, "list");
          if (!list || list->tag() != sdk::Tag::Array) {
            continue;
          }
          std::vector<integration::Interaction> interactions;
          for (const Value& item : list->as_array()) {
            integration::Interaction i;
            const double raw_id = number(item, "id");
            if (!exact_uint32(raw_id)) {
              interactions.clear();
              break;
            }
            i.id = std::uint32_t(raw_id);
            i.entity = text(item, "entity");
            const double raw_body = number(item, "body");
            i.body = exact_uint32(raw_body) ? std::uint32_t(raw_body) : 0;
            if (const auto* p = field(item, "position")) {
              i.position = {float(number(*p, "x")), float(number(*p, "y")), float(number(*p, "z"))};
            }
            i.radius = float(number(item, "radius", 2));
            i.key = std::uint8_t(number(item, "key"));
            const auto* hold = field(item, "hold");
            i.hold = hold && hold->tag() == sdk::Tag::Bool && hold->as_bool();
            i.hold_seconds = float(number(item, "holdSeconds", 1));
            i.target_radius = float(number(item, "targetRadius", 0.6));
            i.target_height = float(number(item, "targetHeight", 1.8));
            i.text = text(item, "text");
            interactions.push_back(std::move(i));
          }
          server_.send_interactions(core::EntityId(number(e, "id")), interactions);
        } else if (op == "dialoguePage") {
          // A conversation's page for one player: { id, session, generation, onRight,
          // line, speaker, options: [{ text, enabled }] }.
          integration::DialoguePage page;
          page.session = std::uint32_t(number(e, "session"));
          page.generation = std::uint32_t(number(e, "generation"));
          page.on_right = flag_of(e, "onRight");
          page.line = text(e, "line");
          page.speaker = text(e, "speaker");
          const auto* options = field(e, "options");
          if (options && options->tag() == sdk::Tag::Array) {
            for (const Value& row : options->as_array()) {
              integration::DialogueOption option;
              option.text = text(row, "text");
              const auto* enabled = field(row, "enabled");
              option.enabled = !enabled || enabled->tag() != sdk::Tag::Bool || enabled->as_bool();
              page.options.push_back(std::move(option));
            }
          }
          if (!server_.send_dialogue_page(core::EntityId(number(e, "id")), page)) {
            std::cerr << "dialogue page of session " << page.session << " reached nobody"
                      << std::endl;
          }
        } else if (op == "dialogueClose") {
          // Takes a conversation off one player's screen: { id, session }.
          integration::DialogueClose close;
          close.session = std::uint32_t(number(e, "session"));
          server_.send_dialogue_close(core::EntityId(number(e, "id")), close);
        } else if (op == "vendorShow") {
          // A vendor's shelf for one player's shop screen: { id, session,
          // open, buys, npc, revision, purse (null: never runs out), money, rows as
          // inventoryShow's, prices: [per row], buyPrices: [[class, price]] }.
          integration::VendorShow show;
          if (!vendor_show_of(e, show)) {
            std::cerr << "vendor show refused for session " << number(e, "session") << std::endl;
            continue;
          }
          if (!server_.send_vendor_show(core::EntityId(number(e, "id")), show)) {
            std::cerr << "vendor show of session " << show.session << " reached nobody"
                      << std::endl;
          }
        } else if (op == "vendorClose") {
          // Takes a vendor's screen off one player: { id, session }.
          integration::VendorClose close;
          close.session = std::uint32_t(number(e, "session"));
          server_.send_vendor_close(core::EntityId(number(e, "id")), close);
        } else if (op == "playerMount") {
          // player.mount(horse): { id, horse, instant }; the player's game mounts.
          integration::MountOrder order;
          const double horse = number(e, "horse");
          order.horse = exact_uint32(horse) ? std::uint32_t(horse) : 0;
          const auto* instant = field(e, "instant");
          order.instant = instant && instant->tag() == sdk::Tag::Bool && instant->as_bool();
          server_.send_mount_order(core::EntityId(number(e, "id")), order);
        } else if (op == "playerDismount") {
          // player.dismount and a mount horseMounting refused: { id }.
          server_.send_dismount_order(core::EntityId(number(e, "id")));
        } else if (op == "cuffs") {
          // Cuffs one player or takes the cuffs off: { id, cuffed }.
          const auto* cuffed = field(e, "cuffed");
          if (cuffed && cuffed->tag() == sdk::Tag::Bool) {
            server_.send_cuffs(core::EntityId(number(e, "id")), cuffed->as_bool());
          }
        } else if (op == "whistle") {
          // A player's client whistles (whistler 0) or hears a nearby player whistle:
          // { id, whistler }.
          server_.send_whistle(core::EntityId(number(e, "id")),
                               std::uint32_t(number(e, "whistler")));
        } else if (op == "interactionRules") {
          // The game's own interactions one player is allowed: { id, allowed } (the bits of
          // kcd2s/integration/interaction_rules.hpp).
          server_.send_interaction_rules(core::EntityId(number(e, "id")),
                                         std::uint16_t(number(e, "allowed")));
        } else if (op == "pillory") {
          // Puts one player into the pillory of a holder entity, or takes them out:
          // { id, kind: 1 put in | 2 release, holder }.
          integration::PilloryOrder order;
          order.kind = std::uint8_t(number(e, "kind"));
          order.holder = text(e, "holder");
          server_.send_pillory(core::EntityId(number(e, "id")), order);
        } else if (op == "appearance") {
          const auto player = core::EntityId(number(e, "id"));
          integration::AppearanceRequest request;
          for (auto [key, part] :
               {std::pair{"gender", &request.gender}, std::pair{"body", &request.body},
                std::pair{"head", &request.head}, std::pair{"hair", &request.hair},
                std::pair{"beard", &request.beard}}) {
            if (field(e, key)) {
              *part = text(e, key);
            }
          }
          if (!server_.request_appearance(player, request))
            std::cerr << "appearance refused for player " << player << std::endl;
        } else if (op == "setStat") {
          // Player.setStat: the host checked the stat and the value; the native server checks them
          // again and refuses a player without a living body.
          const auto player = core::EntityId(number(e, "id"));
          const double stat = number(e, "stat", -1);
          integration::StatRequest request;
          request.stat = std::uint8_t(stat >= 0 && stat < 4 ? stat : 255);
          request.value = float(number(e, "value", -1));
          const auto* fall = field(e, "fall");
          request.fall = fall && fall->tag() == sdk::Tag::Bool && fall->as_bool();
          if (!server_.request_stat(player, request))
            std::cerr << "setStat refused for player " << player << std::endl;
        } else if (op == "addStat") {
          // Player.addStat: { id, stat, delta }; the owner's game adds the change to
          // its current value. The host checked both; the native server checks them again.
          const auto player = core::EntityId(number(e, "id"));
          const double stat = number(e, "stat", -1);
          integration::StatRequest request;
          request.stat = std::uint8_t(stat >= 0 && stat < 4 ? stat : 255);
          request.value = float(number(e, "delta", std::numeric_limits<double>::quiet_NaN()));
          request.relative = true;
          if (!server_.request_stat(player, request))
            std::cerr << "addStat refused for player " << player << std::endl;
        } else if (op == "needs") {
          // Whether one player's game runs the sleep and food needs: { id, enabled }.
          const sdk::Value* enabled = field(e, "enabled");
          server_.send_needs(core::EntityId(number(e, "id")),
                             !enabled || enabled->tag() != sdk::Tag::Bool || enabled->as_bool());
        } else if (op == "progressionOrder") {
          // Player.addXp, setLevel, addPerk, removePerk, addPerkPoints, resetPerks and
          // loadProgression: the host checked the arguments; the codec checks them again.
          const auto player = core::EntityId(number(e, "id"));
          integration::ProgressionOrder order;
          if (!progression_order_of(e, order) || !server_.request_progression_order(player, order))
            std::cerr << "progression order refused for player " << player << std::endl;
        } else if (op == "itemNames") {
          // What the game calls items (Inventory.renameItem), for one player or everyone (0).
          const auto player = core::EntityId(number(e, "id"));
          integration::ItemNames names;
          const auto* list = field(e, "names");
          if (const auto* full = field(e, "full"); full && full->tag() == sdk::Tag::Bool)
            names.full = full->as_bool();
          bool valid = list && list->tag() == sdk::Tag::Array;
          for (const Value& entry : valid ? list->as_array() : Value::Array{}) {
            if (entry.tag() != sdk::Tag::Array || entry.as_array().size() != 2 ||
                entry.as_array()[0].tag() != sdk::Tag::Str ||
                entry.as_array()[1].tag() != sdk::Tag::Str) {
              valid = false;
              break;
            }
            names.names.emplace_back(entry.as_array()[0].as_str(), entry.as_array()[1].as_str());
          }
          if (!valid || !server_.request_item_names(player, names))
            std::cerr << "item names refused" << std::endl;
        } else if (op == "itemOverrides") {
          // What the server changes of the game's own items, for one player or
          // everyone (0): [guid, weight or null, price or null, max or null, [[stat, value]...]]
          // each, the whole set, and the max column's tooltip in the player's language
          // (maxTooltip).
          const auto player = core::EntityId(number(e, "id"));
          integration::ItemOverrides overrides;
          if (const auto* tip = field(e, "maxTooltip"); tip && tip->tag() == sdk::Tag::Str) {
            overrides.max_tooltip = tip->as_str();
          }
          // The share of equipped items' weight in the player's load.
          const double share = number(e, "equippedWeightPercent", 100);
          if (share >= 0 && share <= 100) {
            overrides.equipped_weight_percent = std::uint8_t(share);
          }
          const auto* list = field(e, "items");
          bool valid = list && list->tag() == sdk::Tag::Array;
          for (const Value& entry : valid ? list->as_array() : Value::Array{}) {
            const auto* cells = entry.tag() == sdk::Tag::Array ? &entry.as_array() : nullptr;
            integration::ItemOverride item;
            if (!cells || cells->size() != 5 || (*cells)[0].tag() != sdk::Tag::Str ||
                !integration::parse_guid((*cells)[0].as_str(), item.item) ||
                (*cells)[4].tag() != sdk::Tag::Array) {
              valid = false;
              break;
            }
            for (const Value& pair : (*cells)[4].as_array()) {
              const auto* stat = pair.tag() == sdk::Tag::Array ? &pair.as_array() : nullptr;
              const auto numeric = [](const Value& value) {
                return value.tag() == sdk::Tag::Int || value.tag() == sdk::Tag::Double;
              };
              const auto as_double = [](const Value& value) {
                return value.tag() == sdk::Tag::Double ? value.as_double() : double(value.as_int());
              };
              if (!stat || stat->size() != 2 || !numeric((*stat)[0]) || !numeric((*stat)[1])) {
                valid = false;
                break;
              }
              const double id = as_double((*stat)[0]);
              if (!exact_uint32(id) || id < 1 || id > 255) {
                valid = false;
                break;
              }
              item.stats.push_back(
                  integration::ItemStatValue{std::uint8_t(id), float(as_double((*stat)[1]))});
            }
            if (!valid) {
              break;
            }
            const Value& max = (*cells)[3];
            if (max.tag() == sdk::Tag::Double || max.tag() == sdk::Tag::Int) {
              const double value =
                  max.tag() == sdk::Tag::Double ? max.as_double() : double(max.as_int());
              if (!exact_uint32(value)) {
                valid = false;
                break;
              }
              item.has_max = true;
              item.max = std::uint32_t(value);
            }
            const Value& weight = (*cells)[1];
            const Value& price = (*cells)[2];
            if (weight.tag() == sdk::Tag::Double || weight.tag() == sdk::Tag::Int) {
              item.has_weight = true;
              item.weight = float(weight.tag() == sdk::Tag::Double ? weight.as_double()
                                                                   : double(weight.as_int()));
            }
            if (price.tag() == sdk::Tag::Double || price.tag() == sdk::Tag::Int) {
              const double value =
                  price.tag() == sdk::Tag::Double ? price.as_double() : double(price.as_int());
              if (!exact_uint32(value)) {
                valid = false;
                break;
              }
              item.has_price = true;
              item.price = std::uint32_t(value);
            }
            overrides.items.push_back(item);
          }
          if (!valid || !server_.request_item_overrides(player, overrides))
            std::cerr << "item overrides refused" << std::endl;
        } else if (op == "itemLimitNotice") {
          // A player would take more of a class than its limit: the notice text.
          const auto player = core::EntityId(number(e, "id"));
          integration::ItemLimitNotice notice;
          if (const auto* text = field(e, "text"); text && text->tag() == sdk::Tag::Str) {
            notice.text = text->as_str();
          }
          notice.center = flag_of(e, "center");
          if (!server_.request_item_limit_notice(player, notice))
            std::cerr << "item limit notice refused" << std::endl;
        } else if (op == "itemTypes") {
          // Item types a resource registered (Items.register), for one player or
          // everyone (0): [guid, visual, id, nameKey, descriptionKey, useLabelKey, weight, price,
          // stackable, usable, icon, iconFile, model] each (the icon and the model "" for the
          // visual's).
          const auto player = core::EntityId(number(e, "id"));
          integration::ItemTypes types;
          const auto* list = field(e, "types");
          if (const auto* full = field(e, "full"); full && full->tag() == sdk::Tag::Bool)
            types.full = full->as_bool();
          bool valid = list && list->tag() == sdk::Tag::Array;
          for (const Value& entry : valid ? list->as_array() : Value::Array{}) {
            const auto* cells = entry.tag() == sdk::Tag::Array ? &entry.as_array() : nullptr;
            auto cell_number = [&](std::size_t index) {
              const Value& cell = (*cells)[index];
              return cell.tag() == sdk::Tag::Double ? cell.as_double()
                     : cell.tag() == sdk::Tag::Int  ? double(cell.as_int())
                                                    : -1.0;
            };
            integration::ItemType type;
            if (!cells || cells->size() != 13 || (*cells)[0].tag() != sdk::Tag::Str ||
                (*cells)[1].tag() != sdk::Tag::Str || (*cells)[2].tag() != sdk::Tag::Str ||
                (*cells)[3].tag() != sdk::Tag::Str || (*cells)[4].tag() != sdk::Tag::Str ||
                (*cells)[5].tag() != sdk::Tag::Str || (*cells)[8].tag() != sdk::Tag::Bool ||
                (*cells)[9].tag() != sdk::Tag::Bool || (*cells)[10].tag() != sdk::Tag::Str ||
                (*cells)[11].tag() != sdk::Tag::Str || (*cells)[12].tag() != sdk::Tag::Str ||
                !integration::parse_guid((*cells)[0].as_str(), type.id) ||
                !integration::parse_guid((*cells)[1].as_str(), type.visual) ||
                !exact_uint32(cell_number(7)) || !(cell_number(6) >= 0)) {
              valid = false;
              break;
            }
            type.name = (*cells)[2].as_str();
            type.name_key = (*cells)[3].as_str();
            type.description_key = (*cells)[4].as_str();
            type.use_label_key = (*cells)[5].as_str();
            type.weight = float(cell_number(6));
            type.price = std::uint32_t(cell_number(7));
            type.stackable = (*cells)[8].as_bool();
            type.usable = (*cells)[9].as_bool();
            type.icon = (*cells)[10].as_str();
            type.icon_file = (*cells)[11].as_str();
            type.model = (*cells)[12].as_str();
            types.types.push_back(std::move(type));
          }
          if (!valid || !server_.request_item_types(player, types))
            std::cerr << "item types refused" << std::endl;
        } else if (op == "craftVerdict") {
          // Whether a player's game may open the crafting station it asked about.
          const auto player = core::EntityId(number(e, "id", 0));
          const double request = number(e, "request", -1);
          integration::CraftVerdict verdict;
          verdict.request = std::uint32_t(request);
          verdict.allowed = flag(e, "allowed", false);
          if (!exact_uint32(request) || !server_.send_craft_verdict(player, verdict))
            std::cerr << "craft verdict refused for player " << player << std::endl;
        } else if (op == "gatherVerdict") {
          // Whether a player's pick stands.
          const auto player = core::EntityId(number(e, "id", 0));
          const double request = number(e, "request", -1);
          integration::GatherVerdict verdict;
          verdict.request = std::uint32_t(request);
          verdict.granted = flag(e, "granted", false);
          if (!exact_uint32(request) || !server_.send_gather_verdict(player, verdict))
            std::cerr << "gather verdict refused for player " << player << std::endl;
        } else if (op == "gatherPlants") {
          // Plants picked in a player's world, [id, kind, x, y, z, radius] each, and the ids of
          // picks grown back; full replaces what the player's game was told before.
          const auto player = core::EntityId(number(e, "id", 0));
          integration::GatherPlants plants;
          plants.full = flag(e, "full", false);
          const auto numeric = [](const Value& value) {
            return value.tag() == sdk::Tag::Int || value.tag() == sdk::Tag::Double;
          };
          const auto as_double = [](const Value& value) {
            return value.tag() == sdk::Tag::Double ? value.as_double() : double(value.as_int());
          };
          bool valid = true;
          const auto* picked = field(e, "picked");
          for (const Value& entry :
               picked && picked->tag() == sdk::Tag::Array ? picked->as_array() : Value::Array{}) {
            const auto* cells = entry.tag() == sdk::Tag::Array ? &entry.as_array() : nullptr;
            if (!cells || cells->size() != 6) {
              valid = false;
              break;
            }
            for (const Value& cell : *cells) {
              valid = valid && numeric(cell);
            }
            if (!valid || !exact_uint32(as_double((*cells)[0])) ||
                !exact_uint32(as_double((*cells)[1])) || as_double((*cells)[1]) > 255) {
              valid = false;
              break;
            }
            integration::GatherPick pick;
            pick.id = std::uint32_t(as_double((*cells)[0]));
            pick.kind = std::uint8_t(as_double((*cells)[1]));
            pick.position = {float(as_double((*cells)[2])), float(as_double((*cells)[3])),
                             float(as_double((*cells)[4]))};
            pick.radius = float(as_double((*cells)[5]));
            plants.picked.push_back(pick);
          }
          const auto* grown = field(e, "grown");
          for (const Value& id :
               grown && grown->tag() == sdk::Tag::Array ? grown->as_array() : Value::Array{}) {
            if (!numeric(id) || !exact_uint32(as_double(id))) {
              valid = false;
              break;
            }
            plants.grown.push_back(std::uint32_t(as_double(id)));
          }
          if (!valid || !server_.send_gather_plants(player, plants))
            std::cerr << "gather plants refused for player " << player << std::endl;
        } else if (op == "potLooks") {
          // How stew pots of a player's world look (cookpot/cookpots.cjs): each
          // [cauldron as 16 hex digits, camp, phase, fireplace as 16 hex digits or "", fire (0
          // the game's own, 1 lit, 2 put out)].
          const auto player = core::EntityId(number(e, "id", 0));
          integration::PotLooks looks;
          bool valid = true;
          const auto hex_guid = [](const std::string& digits, std::uint64_t& guid) {
            if (digits.size() != 16 ||
                digits.find_first_not_of("0123456789abcdef") != std::string::npos) {
              return false;
            }
            guid = std::stoull(digits, nullptr, 16);
            return true;
          };
          const auto* entries = field(e, "looks");
          for (const Value& entry : entries && entries->tag() == sdk::Tag::Array
                                        ? entries->as_array()
                                        : Value::Array{}) {
            const auto* cells = entry.tag() == sdk::Tag::Array ? &entry.as_array() : nullptr;
            if (!cells || cells->size() < 4 || cells->size() > 5 ||
                (*cells)[0].tag() != sdk::Tag::Str || (*cells)[1].tag() != sdk::Tag::Bool ||
                (*cells)[3].tag() != sdk::Tag::Str ||
                ((*cells)[2].tag() != sdk::Tag::Int && (*cells)[2].tag() != sdk::Tag::Double)) {
              valid = false;
              break;
            }
            // A script's number may come as a double.
            const double phase = (*cells)[2].tag() == sdk::Tag::Double
                                     ? (*cells)[2].as_double()
                                     : double((*cells)[2].as_int());
            integration::PotLook look;
            if (!hex_guid((*cells)[0].as_str(), look.cauldron) || phase != std::floor(phase) ||
                phase < integration::kMinPotPhase || phase > integration::kMaxPotPhase) {
              valid = false;
              break;
            }
            const std::string& fireplace = (*cells)[3].as_str();
            if (!fireplace.empty() && !hex_guid(fireplace, look.fireplace)) {
              valid = false;
              break;
            }
            look.camp = (*cells)[1].as_bool();
            look.phase = std::uint8_t(phase);
            if (cells->size() == 5) {
              const Value& fire = (*cells)[4];
              const double code = fire.tag() == sdk::Tag::Int      ? double(fire.as_int())
                                  : fire.tag() == sdk::Tag::Double ? fire.as_double()
                                                                   : -1.0;
              if (code != std::floor(code) || code < 0 || code > integration::kPotFireOut) {
                valid = false;
                break;
              }
              look.fire = std::uint8_t(code);
            }
            looks.looks.push_back(look);
          }
          if (!valid || !server_.send_pot_looks(player, looks))
            std::cerr << "pot looks refused for player " << player << std::endl;
        } else if (op == "diceTables") {
          // Whether a resource answers a dice table's use (dice/dice.cjs).
          integration::DiceTables tables;
          tables.offered = flag(e, "offered", false);
          if (!server_.send_dice_tables(core::EntityId(number(e, "id", 0)), tables))
            std::cerr << "dice tables refused for player " << number(e, "id", 0) << std::endl;
        } else if (op == "diceWatch") {
          // A dice match as a player nearby sees it (dice/dice.cjs): the table's
          // things, [key as 16 hex digits, x, y, z, qx, qy, qz, qw, shown, scale, model,
          // material] each, or ended.
          const auto player = core::EntityId(number(e, "id", 0));
          integration::DiceWatch watch;
          const double match = number(e, "match", -1);
          bool valid = exact_uint32(match);
          watch.match = std::uint32_t(valid ? match : 0);
          watch.ended = flag(e, "ended", false);
          const auto numeric = [](const Value& value) {
            return value.tag() == sdk::Tag::Int || value.tag() == sdk::Tag::Double;
          };
          const auto as_double = [](const Value& value) {
            return value.tag() == sdk::Tag::Double ? value.as_double() : double(value.as_int());
          };
          const auto* things = field(e, "things");
          for (const Value& entry :
               things && things->tag() == sdk::Tag::Array ? things->as_array() : Value::Array{}) {
            const auto* cells = entry.tag() == sdk::Tag::Array ? &entry.as_array() : nullptr;
            if (!cells || cells->size() != 12 || (*cells)[0].tag() != sdk::Tag::Str ||
                (*cells)[8].tag() != sdk::Tag::Bool || !numeric((*cells)[9]) ||
                (*cells)[10].tag() != sdk::Tag::Str || (*cells)[11].tag() != sdk::Tag::Str) {
              valid = false;
              break;
            }
            const std::string& key = (*cells)[0].as_str();
            if (key.size() != 16 ||
                key.find_first_not_of("0123456789abcdef") != std::string::npos) {
              valid = false;
              break;
            }
            integration::DiceThing thing;
            thing.key = std::stoull(key, nullptr, 16);
            for (std::size_t i = 0; i < 7; ++i) {
              if (!numeric((*cells)[1 + i])) {
                valid = false;
                break;
              }
              const float value = float(as_double((*cells)[1 + i]));
              if (i < 3) {
                thing.position[i] = value;
              } else {
                thing.rotation[i - 3] = value;
              }
            }
            thing.shown = (*cells)[8].as_bool();
            thing.scale = float(as_double((*cells)[9]));
            thing.model = (*cells)[10].as_str();
            thing.material = (*cells)[11].as_str();
            watch.things.push_back(thing);
          }
          if (!valid || !server_.send_dice_watch(player, watch))
            std::cerr << "dice watch refused for player " << player << std::endl;
        } else if (op == "diceState") {
          // A dice match as one of its players sees it (dice/dice.cjs): kind start
          // (opponent, behavior as 16 hex digits, seat, first, target, ownName, opponentName,
          // ownDice and opponentDice: none or six meshes),
          // names (ownName, opponentName), sit (behavior, seat: the player sits down at the
          // table and waits, match 0), roll
          // (mine, faces [6], rolled, bust), move (move continue or pass, held) or end (reason,
          // result won, lost or none).
          const auto player = core::EntityId(number(e, "id", 0));
          const std::string kind = text(e, "kind");
          const double match = number(e, "match", -1);
          integration::DiceState state;
          state.match = std::uint32_t(match);
          bool valid = exact_uint32(match);
          const auto hex_guid = [](const std::string& digits, std::uint64_t& guid) {
            if (digits.size() != 16 ||
                digits.find_first_not_of("0123456789abcdef") != std::string::npos) {
              return false;
            }
            guid = std::stoull(digits, nullptr, 16);
            return true;
          };
          if (kind == "start") {
            state.kind = integration::DiceState::Kind::Start;
            const double opponent = number(e, "opponent", -1);
            const double target = number(e, "target", -1);
            const double seat = number(e, "seat", -1);
            valid = valid && exact_uint32(opponent) && exact_uint32(target) && exact_uint32(seat) &&
                    seat <= 1 && hex_guid(text(e, "behavior"), state.behavior);
            if (valid) {
              state.opponent = std::uint32_t(opponent);
              state.target = std::uint32_t(target);
              state.seat = std::uint8_t(seat);
            }
            state.first = flag(e, "first", false);
            state.own_name = text(e, "ownName");
            state.opponent_name = text(e, "opponentName");
            const auto meshes = [&](const char* key, std::vector<std::string>& dice) {
              const auto* list = field(e, key);
              if (!list) {
                return;
              }
              if (list->tag() != sdk::Tag::Array) {
                valid = false;
                return;
              }
              for (const Value& mesh : list->as_array()) {
                if (mesh.tag() != sdk::Tag::Str) {
                  valid = false;
                  return;
                }
                dice.push_back(mesh.as_str());
              }
            };
            meshes("ownDice", state.own_dice);
            meshes("opponentDice", state.opponent_dice);
            // Each die's six side values ([[0..6] x6] per die), as many as the meshes.
            const auto sides = [&](const char* key, std::vector<std::array<std::uint8_t, 6>>& out) {
              const auto* list = field(e, key);
              if (!list) {
                return;
              }
              if (list->tag() != sdk::Tag::Array) {
                valid = false;
                return;
              }
              for (const Value& die : list->as_array()) {
                if (die.tag() != sdk::Tag::Array || die.as_array().size() != 6) {
                  valid = false;
                  return;
                }
                std::array<std::uint8_t, 6> values{};
                for (std::size_t i = 0; i < 6; ++i) {
                  // A whole number 0 to 6 (script numbers may arrive as doubles).
                  const Value& side = die.as_array()[i];
                  double number = -1;
                  if (side.tag() == sdk::Tag::Int) {
                    number = double(side.as_int());
                  } else if (side.tag() == sdk::Tag::Double) {
                    number = side.as_double();
                  }
                  if (number < 0 || number > 6 || number != double(std::int64_t(number))) {
                    valid = false;
                    return;
                  }
                  values[i] = std::uint8_t(number);
                }
                out.push_back(values);
              }
            };
            sides("ownSides", state.own_sides);
            sides("opponentSides", state.opponent_sides);
            // A badge: {item: its class id, effect: 0 to 12, tier: 0 to 2}; null or missing for
            // none.
            const auto badge = [&](const char* key, integration::DiceBadge& out) {
              const auto* entry = field(e, key);
              if (!entry || entry->tag() == sdk::Tag::Null) {
                return;
              }
              if (entry->tag() != sdk::Tag::Map) {
                valid = false;
                return;
              }
              const double effect = number(*entry, "effect", -1);
              const double tier = number(*entry, "tier", -1);
              if (!integration::parse_guid(text(*entry, "item"), out.item) ||
                  !exact_uint32(effect) || effect >= integration::kDiceBadgeEffects ||
                  !exact_uint32(tier) || tier >= integration::kDiceBadgeTiers) {
                valid = false;
                return;
              }
              out.effect = std::uint8_t(effect);
              out.tier = std::uint8_t(tier);
            };
            badge("ownBadge", state.own_badge);
            badge("opponentBadge", state.opponent_badge);
          } else if (kind == "sit") {
            state.kind = integration::DiceState::Kind::Sit;
            const double seat = number(e, "seat", -1);
            valid = valid && state.match == 0 && exact_uint32(seat) && seat <= 1 &&
                    hex_guid(text(e, "behavior"), state.behavior);
            state.seat = std::uint8_t(valid ? seat : 0);
          } else if (kind == "names") {
            state.kind = integration::DiceState::Kind::Names;
            state.own_name = text(e, "ownName");
            state.opponent_name = text(e, "opponentName");
          } else if (kind == "roll") {
            state.kind = integration::DiceState::Kind::Roll;
            state.mine = flag(e, "mine", false);
            state.bust = flag(e, "bust", false);
            const double rolled = number(e, "rolled", -1);
            valid = valid && exact_uint32(rolled) && rolled <= integration::kSevenDice;
            state.rolled = std::uint8_t(valid ? rolled : 0);
            const auto* faces = field(e, "faces");
            valid = valid && faces && faces->tag() == sdk::Tag::Array &&
                    faces->as_array().size() == state.faces.size();
            for (std::size_t die = 0; valid && die < state.faces.size(); ++die) {
              const Value& face = faces->as_array()[die];
              const double value = face.tag() == sdk::Tag::Double ? face.as_double()
                                   : face.tag() == sdk::Tag::Int  ? double(face.as_int())
                                                                  : -1;
              // 1 to 6, or the joker side of a joker die (integration::kDiceJoker).
              valid = exact_uint32(value) && value <= integration::kDiceJoker;
              state.faces[die] = std::uint8_t(valid ? value : 0);
            }
          } else if (kind == "move") {
            state.kind = integration::DiceState::Kind::Move;
            const std::string move = text(e, "move");
            state.move = move == "pass"         ? integration::DiceState::Move::Pass
                         : move == "select"     ? integration::DiceState::Move::Select
                         : move == "badge"      ? integration::DiceState::Move::Badge
                         : move == "badgePass"  ? integration::DiceState::Move::BadgePass
                         : move == "bustChoice" ? integration::DiceState::Move::BustChoice
                                                : integration::DiceState::Move::Continue;
            const double held = number(e, "held", -1);
            valid = valid && exact_uint32(held) && held <= integration::kSevenDice;
            state.held = std::uint8_t(valid ? held : 0);
          } else if (kind == "end") {
            state.kind = integration::DiceState::Kind::End;
            const std::string reason = text(e, "reason");
            const std::string result = text(e, "result");
            state.reason = reason == "won"        ? integration::DiceEnd::Won
                           : reason == "gaveUp"   ? integration::DiceEnd::GaveUp
                           : reason == "left"     ? integration::DiceEnd::Left
                           : reason == "timedOut" ? integration::DiceEnd::TimedOut
                           : reason == "failed"   ? integration::DiceEnd::Failed
                                                  : integration::DiceEnd::Stopped;
            state.result = result == "won"    ? integration::DiceResult::Won
                           : result == "lost" ? integration::DiceResult::Lost
                                              : integration::DiceResult::NoWinner;
          } else {
            valid = false;
          }
          if (!valid || !server_.send_dice_state(player, state))
            std::cerr << "dice state refused for player " << player << std::endl;
        } else if (op == "inventoryConsumeReply") {
          // The verdict on one of a player's uses: refused, native or plain.
          const auto player = core::EntityId(number(e, "id", 0));
          const double request = number(e, "request", -1);
          const std::string verdict = text(e, "verdict");
          integration::InventoryConsume consume;
          consume.request = std::uint32_t(request);
          consume.verdict = verdict == "native"  ? integration::InventoryConsume::Verdict::Native
                            : verdict == "plain" ? integration::InventoryConsume::Verdict::Plain
                                                 : integration::InventoryConsume::Verdict::Refused;
          if (!exact_uint32(request) || !server_.request_inventory_consume(player, consume))
            std::cerr << "inventory consume reply refused for player " << player << std::endl;
        } else if (op == "inventoryShow") {
          // A player's whole inventory for their game to show.
          const auto player = core::EntityId(number(e, "id"));
          integration::InventoryShow show;
          if (!inventory_show_of(e, show) || !server_.request_inventory_show(player, show))
            std::cerr << "inventory show refused for player " << player << std::endl;
        } else if (op == "inventoryChangesAck") {
          // The scripts' answer to a batch of changes a player's game made.
          const auto player = core::EntityId(number(e, "id"));
          const double epoch = number(e, "epoch");
          const double sequence = number(e, "sequence");
          const double revision = number(e, "revision");
          integration::InventoryChangesAck ack;
          ack.epoch = std::uint32_t(epoch);
          ack.sequence = std::uint32_t(sequence);
          ack.revision = revision >= 0 ? std::uint64_t(revision) : 0;
          ack.verdict = text(e, "verdict") == "applied"
                            ? integration::InventoryChangesAck::Verdict::Applied
                            : integration::InventoryChangesAck::Verdict::Refused;
          if (!exact_uint32(epoch) || !exact_uint32(sequence) ||
              !server_.request_inventory_changes_ack(player, ack))
            std::cerr << "inventory changes answer refused for player " << player << std::endl;
        } else if (op == "buffOrder") {
          // Player.addBuff, removeBuff, clearBuffs and Buffs.claim: the host checked the
          // arguments; the codec checks them again.
          const auto player = core::EntityId(number(e, "id"));
          integration::BuffOrder order;
          if (!buff_order_of(e, order) || !server_.request_buff_order(player, order))
            std::cerr << "buff order refused for player " << player << std::endl;
        } else if (op == "customBuff") {
          // Buffs.register's buffs on a player's status list: the host keeps them, the codec
          // checks what travels.
          const auto player = core::EntityId(number(e, "id"));
          integration::CustomBuff buff;
          if (!custom_buff_of(e, buff) || !server_.request_custom_buff(player, buff))
            std::cerr << "custom buff refused for player " << player << std::endl;
        } else if (op == "movementMode") {
          // Player.setPace: the whole rule, a key left out off.
          const auto player = core::EntityId(number(e, "id"));
          integration::MovementMode mode;
          const auto* by_default = field(e, "walkByDefault");
          const auto* enforced = field(e, "walkEnforced");
          mode.walk_by_default =
              by_default && by_default->tag() == sdk::Tag::Bool && by_default->as_bool();
          mode.walk_enforced = enforced && enforced->tag() == sdk::Tag::Bool && enforced->as_bool();
          if (!server_.request_movement_mode(player, mode))
            std::cerr << "movement mode refused for player " << player << std::endl;
        } else if (op == "visible") {
          const auto player = core::EntityId(number(e, "id"));
          const auto* visible = field(e, "visible");
          if (!visible || visible->tag() != sdk::Tag::Bool ||
              !server_.request_body_visible(player, visible->as_bool()))
            std::cerr << "visibility refused for player " << player << std::endl;
        } else if (op == "disguise") {
          const auto player = core::EntityId(number(e, "id"));
          if (!server_.request_disguise(player, text(e, "model")))
            std::cerr << "disguise refused for player " << player << std::endl;

        } else if (op == "chat") {
          const auto target = core::EntityId(number(e, "target"));
          server_.send_chat(
              target, {text(e, "author"), text(e, "text"), std::uint32_t(number(e, "color"))});
        } else if (op == "playerConnectingUpdate") {
          // connection.update(message): a line for the waiting player, and a new timeout.
          const double request = number(e, "request");
          for (auto& [source, admission] : admissions_) {
            (void)source;
            if (exact_uint32(request) && admission.request == std::uint32_t(request) &&
                !admission.decided) {
              admission.line = transport::sanitize_refusal_text(text(e, "message"));
              admission.started = clock_seconds();
            }
          }
        } else if (op == "joinQueueSet") {
          // Queue.setPriority / setStatus / reject: { id, priority?, line?, fields?,
          // reject?, reason? }.
          const double id = number(e, "id");
          QueueEntry* entry = exact_uint32(id) ? queue_entry(std::uint32_t(id)) : nullptr;
          if (!entry) {
            continue;
          }
          if (const sdk::Value* priority = field(e, "priority")) {
            const double value = number(e, "priority");
            if (priority->tag() != sdk::Tag::Null && std::isfinite(value)) {
              entry->priority = std::int64_t(value);
            }
          }
          if (field(e, "line")) {
            entry->line = transport::sanitize_refusal_text(text(e, "line"));
          }
          if (const sdk::Value* fields = field(e, "fields")) {
            entry->fields.clear();
            if (fields->tag() == sdk::Tag::Array) {
              for (const Value& pair : fields->as_array()) {
                if (pair.tag() == sdk::Tag::Array && pair.as_array().size() == 2 &&
                    pair.as_array()[0].tag() == sdk::Tag::Str &&
                    pair.as_array()[1].tag() == sdk::Tag::Str &&
                    entry->fields.size() < transport::kMaxWaitFields) {
                  entry->fields.push_back(
                      {pair.as_array()[0].as_str(), pair.as_array()[1].as_str()});
                }
              }
            }
          }
          if (flag_of(e, "reject")) {
            entry->rejected = true;
            entry->reason = transport::sanitize_refusal_text(text(e, "reason"));
          }
        } else if (op == "playerConnectingResult") {
          const double request = number(e, "request");
          if (!exact_uint32(request))
            continue;
          const auto* allowed = field(e, "allowed");
          const auto* password = field(e, "password");
          decide_admission(std::uint32_t(request),
                           allowed && allowed->tag() == sdk::Tag::Bool && allowed->as_bool(),
                           text(e, "reason"),
                           password && password->tag() == sdk::Tag::Bool && password->as_bool(),
                           flag_of(e, "banned"), flag_of(e, "discord"));
        } else if (op == "entityIdsRequest") {
          const auto first = reserve_script_ids();
          if (first == core::kInvalidEntity) {
            std::cerr << "RESOURCE entity ids exhausted" << std::endl;
            continue;
          }
          scripts_->send(Value(Value::Map{{"op", Value("entityIds")},
                                          {"first", Value(std::int64_t(first))},
                                          {"count", Value(std::int64_t(kScriptEntityIdBlock))}}));
        } else if (op == "npcSpawn" || op == "npcDestroy" || op == "npcName" || op == "npcFrozen" ||
                   op == "npcDefense" || op == "npcDamage" || op == "npcWorld" ||
                   op == "npcTeleport" || op == "npcAnimation" || op == "npcIntent" ||
                   op == "npcSay" || op == "npcAppearance" || op == "npcOutfit" ||
                   op == "npcHealth" || op == "npcNameplate" || op == "npcLocomotion" ||
                   op == "npcBrain" || op == "npcTargetable" || op == "npcAttackable" ||
                   op == "npcInteractable" || op == "npcNameShown" || op == "npcLevelGuid" ||
                   op == "npcCarcassClosed" || op == "npcTalking" || op == "npcFace" ||
                   op == "npcPin") {
          npc_op(op, e);
        } else if (op == "horseSpawn") {
          // The resource host named the horse with an id the world reserved for it, so the same id
          // is the horse's network id in scripts, on the wire and on every client.
          const double raw_id = number(e, "id");
          if (!exact_uint32(raw_id))
            continue;
          const auto script_id = std::uint32_t(raw_id);
          if (horses_.count(script_id))
            continue;
          auto pv = field(e, "position"), qv = field(e, "rotation");
          if (!pv || !qv)
            continue;
          core::Vec3 p{float(number(*pv, "x")), float(number(*pv, "y")), float(number(*pv, "z"))};
          core::Quat q{float(number(*qv, "x")), float(number(*qv, "y")), float(number(*qv, "z")),
                       float(number(*qv, "w", 1))};
          if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
              std::abs(p.x) > 1e6f || std::abs(p.y) > 1e6f || std::abs(p.z) > 1e6f)
            continue;
          auto id = server_.world().spawn_reserved(core::type_id("NetworkHorse"), script_id);
          if (!id) {
            std::cerr << "RESOURCE horse rejected: id " << script_id << " was not reserved"
                      << std::endl;
            continue;
          }
          horses_[script_id] = id;
          auto& w = server_.world();
          w.write(id, 0, p);
          w.write(id, 1, q);
          w.write(id, 2, text(e, "soul"));
          w.write(id, 3, text(e, "name"));
          w.write(id, 4, std::int64_t(script_id));
          for (core::FieldId f = 5; f <= 7; ++f)
            w.write(id, f, std::int64_t(f == 6 ? replication::kGlobalWorld : 0));
          // The look the resource host chose; "" leaves the horse to each client.
          const std::string look = text(e, "look");
          w.write(id, integration::HorseLook,
                  integration::valid_component_names(look, 4) ? look : std::string{});
          // Whether a lethal blow knocks the horse out instead of killing it.
          w.write(id, integration::HorseKnockOut, flag_of(e, "knockOut"));
          w.write(id, integration::HorseIntent, std::string{});
          // A new horse is alive at full health, in its first life.
          w.write(id, integration::HorseHealth, integration::kHorseMaxHealth);
          w.write(id, integration::HorseCondition, integration::kHorseAlive);
          w.write(id, integration::HorseLife, std::int64_t(0));
          // The most its saddlebags carry; below 0 the game's own.
          const double carry = number(e, "maxCarryWeight", -1);
          w.write(id, integration::HorseMaxCarryWeight,
                  float(std::isfinite(carry) && carry >= 0 && carry <= 100000 ? carry : -1));
          // Its stamina and speed; below 0 the game's own.
          const double stamina = number(e, "maxStamina", -1);
          const double speed = number(e, "speed", -1);
          w.write(id, integration::HorseMaxStamina,
                  float(std::isfinite(stamina) && stamina > 0 && stamina <= 10000 ? stamina : -1));
          w.write(id, integration::HorseSpeed,
                  float(std::isfinite(speed) && speed >= 30 && speed <= 60 ? speed : -1));
          // Its tack comes with the horseGear that follows.
          w.write(id, integration::HorseGear, std::string{});
          // It carries no buff of the server's yet (Horse.addBuff).
          w.write(id, integration::HorseBuffs, std::string{});
          // Nobody but its owner rides it yet (Horse.allowRider).
          w.write(id, integration::HorseRiders, std::string{});
          // Nobody leads it on a rope (Horse.follow with rope).
          w.write(id, integration::HorseLead, std::int64_t(0));
          // Its dirt is the game's own (Horse.dirt).
          w.write(id, integration::HorseDirt, -1.0f);
          // Its pace is the game's own (Horse.speedScale).
          w.write(id, integration::HorseSpeedScale, -1.0f);
          w.write(id, integration::HorseLeadAt, std::string{});
          std::cout << "HORSE resource spawn script=" << script_id << " network=" << id
                    << " pos=" << p.x << "," << p.y << "," << p.z << std::endl;
        } else if (op == "groundItemSpawn") {
          // A stack laid in the world (GroundItem), under an id the world reserved.
          const double raw_id = number(e, "id");
          const double amount = number(e, "amount", 0);
          const double quality = number(e, "quality", 0);
          const double health = number(e, "health", -1);
          const double world = number(e, "dimension", double(replication::kGlobalWorld));
          const std::string item_class = text(e, "itemClass");
          std::array<std::uint8_t, 16> class_bytes{};
          auto pv = field(e, "position"), qv = field(e, "rotation");
          if (!exact_uint32(raw_id) || ground_items_.count(std::uint32_t(raw_id)) || !pv || !qv ||
              !integration::parse_item_class_script_text(item_class, class_bytes) ||
              !(amount >= 1 && amount <= integration::kMaxGroundAmount) ||
              std::floor(amount) != amount || !(quality >= 0 && quality <= 255) ||
              !(health == -1 || (health >= 0 && health <= 1)) || !exact_uint32(world)) {
            std::cerr << "RESOURCE ground item refused: malformed spawn " << raw_id << std::endl;
            continue;
          }
          core::Vec3 p{float(number(*pv, "x")), float(number(*pv, "y")), float(number(*pv, "z"))};
          core::Quat q{float(number(*qv, "x")), float(number(*qv, "y")), float(number(*qv, "z")),
                       float(number(*qv, "w", 1))};
          if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
              std::abs(p.x) > 1e6f || std::abs(p.y) > 1e6f || std::abs(p.z) > 1e6f) {
            std::cerr << "RESOURCE ground item refused: position " << raw_id << std::endl;
            continue;
          }
          const auto script_id = std::uint32_t(raw_id);
          auto id = server_.world().spawn_reserved(core::type_id("NetworkGroundItem"), script_id);
          if (!id) {
            std::cerr << "RESOURCE ground item rejected: id " << script_id << " was not reserved"
                      << std::endl;
            continue;
          }
          ground_items_[script_id] = id;
          auto& w = server_.world();
          w.write(id, integration::GroundPosition, p);
          w.write(id, integration::GroundRotation, q);
          w.write(id, integration::GroundItemClass, item_class);
          w.write(id, integration::GroundAmount, std::int64_t(amount));
          w.write(id, integration::GroundQuality, std::int64_t(quality));
          w.write(id, integration::GroundHealth, float(health));
          // Whether it lies where it stays; false while the game's physics may still move it
          // the clients in view report where it comes to rest.
          w.write(id, integration::GroundResting, flag_of(e, "resting"));
          w.write(id, integration::GroundDroppedBy, std::int64_t(number(e, "leftBy", 0)));
          w.write(id, integration::GroundScriptId, std::int64_t(script_id));
          w.write(id, integration::GroundDimension, std::int64_t(world));
          // Held where it lies, without a body: an arrow stuck in a target.
          w.write(id, integration::GroundPinned, flag_of(e, "pinned"));
          if (std::uint32_t(world) != replication::kGlobalWorld)
            server_.set_dimension(id, std::uint32_t(world));
          std::cout << "GROUND stack spawn script=" << script_id << " network=" << id << " "
                    << item_class << " x" << std::int64_t(amount) << std::endl;
        } else if (op == "stashSpawn") {
          // A container the server holds (Stash), under an id the world
          // reserved; bodyOf names the knocked-out player whose body it is.
          const double raw_id = number(e, "id");
          const double body_of = number(e, "bodyOf", 0);
          const double world = number(e, "dimension", double(replication::kGlobalWorld));
          auto pv = field(e, "position"), qv = field(e, "rotation");
          // An adopted level chest: the level entity every client stands it as.
          const std::string level_guid = text(e, "placedGuid");
          // The mesh a script built it as, empty for the default chest.
          const std::string model = text(e, "model");
          if (!exact_uint32(raw_id) || stashes_.count(std::uint32_t(raw_id)) || !pv || !qv ||
              !exact_uint32(body_of) || !exact_uint32(world) ||
              !integration::stash_level_guid_valid(level_guid) ||
              !integration::stash_model_valid(model)) {
            std::cerr << "RESOURCE stash refused: malformed spawn " << raw_id << std::endl;
            continue;
          }
          core::Vec3 p{float(number(*pv, "x")), float(number(*pv, "y")), float(number(*pv, "z"))};
          core::Quat q{float(number(*qv, "x")), float(number(*qv, "y")), float(number(*qv, "z")),
                       float(number(*qv, "w", 1))};
          if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
              std::abs(p.x) > 1e6f || std::abs(p.y) > 1e6f || std::abs(p.z) > 1e6f) {
            std::cerr << "RESOURCE stash refused: position " << raw_id << std::endl;
            continue;
          }
          const auto script_id = std::uint32_t(raw_id);
          auto id = server_.world().spawn_reserved(core::type_id("NetworkStash"), script_id);
          if (!id) {
            std::cerr << "RESOURCE stash rejected: id " << script_id << " was not reserved"
                      << std::endl;
            continue;
          }
          stashes_[script_id] = id;
          auto& w = server_.world();
          w.write(id, integration::StashPosition, p);
          w.write(id, integration::StashRotation, q);
          w.write(id, integration::StashScriptId, std::int64_t(script_id));
          w.write(id, integration::StashBodyOf, std::int64_t(body_of));
          w.write(id, integration::StashDimension, std::int64_t(world));
          // Unlocked, as the game's own chest is built (stashLock changes it).
          w.write(id, integration::StashLocked, false);
          w.write(id, integration::StashLockpickable, true);
          w.write(id, integration::StashKeyClass, std::string());
          w.write(id, integration::StashLevelGuid, level_guid);
          w.write(id, integration::StashLockDifficulty, -1.0f);
          w.write(id, integration::StashLockFanciness, integration::kLockFancinessLevel);
          w.write(id, integration::StashModel, model);
          w.write(id, integration::StashEmpty, true);
          if (std::uint32_t(world) != replication::kGlobalWorld)
            server_.set_dimension(id, std::uint32_t(world));
          std::cout << "STASH spawn script=" << script_id << " network=" << id
                    << " bodyOf=" << std::int64_t(body_of) << std::endl;
        } else if (op == "stashLock") {
          // A scripted container's lock: { id, locked, canBePicked, keyClass }. It
          // rides on the container's own fields, so every client that streams it in, a late
          // joiner too, applies it to the game's lock.
          const auto found = stashes_.find(std::uint32_t(number(e, "id")));
          const std::string key_class = text(e, "keyClass");
          if (found == stashes_.end() || key_class.size() > 64) {
            std::cerr << "RESOURCE stash lock refused: unknown " << number(e, "id") << std::endl;
            continue;
          }
          auto& w = server_.world();
          w.write(found->second, integration::StashLocked, flag_of(e, "locked"));
          w.write(found->second, integration::StashLockpickable, flag_of(e, "canBePicked"));
          w.write(found->second, integration::StashKeyClass, key_class);
          // The lock-picking difficulty: 0 to 1, below 0 the game's own.
          const float difficulty = float(number(e, "lockLevel", -1));
          w.write(found->second, integration::StashLockDifficulty,
                  integration::lock_difficulty_valid(difficulty) ? difficulty : -1.0f);
          // Which lock the minigame shows: "common", "fancy" or "" for the level's.
          const std::string fanciness = text(e, "lockStyle");
          w.write(found->second, integration::StashLockFanciness,
                  fanciness == "common"  ? integration::kLockFancinessCommon
                  : fanciness == "fancy" ? integration::kLockFancinessFancy
                                         : integration::kLockFancinessLevel);
          std::cout << "STASH lock script=" << found->first
                    << " locked=" << (flag_of(e, "locked") ? 1 : 0) << std::endl;
        } else if (op == "levelEditApply") {
          // A level edit: { id, kind, name, guid, at, hidden, position, rotation,
          // scale, dimension, to? }. Without `to` every ready player of its world hears it;
          // with `to` (a replay when that player's world got ready) only that player.
          integration::LevelEdit edit;
          std::uint32_t world = replication::kGlobalWorld;
          if (!level_edit_of(e, edit, world)) {
            std::cerr << "RESOURCE level edit refused: malformed " << number(e, "id") << std::endl;
            continue;
          }
          const auto to = core::EntityId(number(e, "to", 0));
          if (to == 0) {
            level_edits_[edit.id] = {edit, world};
          }
          server_.send_level_edit(to, edit, world);
        } else if (op == "levelEditRemove") {
          // A level edit is gone: { id }; every ready client restores the object.
          const double raw_id = number(e, "id");
          if (!exact_uint32(raw_id) || raw_id < 1) {
            std::cerr << "RESOURCE level edit remove refused: " << raw_id << std::endl;
            continue;
          }
          level_edits_.erase(std::uint32_t(raw_id));
          server_.send_level_edit_remove(0, std::uint32_t(raw_id));
        } else if (op == "stashDestroy") {
          auto found = stashes_.find(std::uint32_t(number(e, "id")));
          if (found == stashes_.end()) {
            std::cerr << "RESOURCE stash destroy: unknown " << number(e, "id") << std::endl;
            continue;
          }
          std::cout << "STASH destroy script=" << found->first << " network=" << found->second
                    << std::endl;
          server_.despawn(found->second);
          stashes_.erase(found);
        } else if (op == "stashShow") {
          // What a container holds, for the player holding it: { id (the player), stash, open,
          // closed, revision, rows as inventoryShow's, horse (a horse's saddlebags) }.
          const auto player = core::EntityId(number(e, "id"));
          const double stash = number(e, "stash", 0);
          const double horse = number(e, "horse", 0);
          const auto* open = field(e, "open");
          const auto* closed = field(e, "closed");
          integration::InventoryShow stock;
          integration::StashShow show;
          if (!exact_uint32(stash) || stash == 0 || !exact_uint32(horse) ||
              !inventory_show_of(e, stock) || stock.rows.size() > integration::kMaxStashRows) {
            std::cerr << "stash show refused for player " << player << std::endl;
            continue;
          }
          show.stash = std::uint32_t(stash);
          show.open = open && open->tag() == sdk::Tag::Bool && open->as_bool();
          show.closed = closed && closed->tag() == sdk::Tag::Bool && closed->as_bool();
          show.revision = stock.revision;
          show.rows = std::move(stock.rows);
          show.horse = std::uint32_t(horse);
          if (!server_.request_stash_show(player, show))
            std::cerr << "stash show refused for player " << player << std::endl;
        } else if (op == "groundItemRest") {
          // Where the stack came to rest: every client lays it there.
          auto found = ground_items_.find(std::uint32_t(number(e, "id")));
          std::array<float, 3> p{};
          std::array<float, 4> q{};
          if (found == ground_items_.end() || !floats_of(e, "position", p) ||
              !floats_of(e, "rotation", q) || !std::isfinite(p[0]) || !std::isfinite(p[1]) ||
              !std::isfinite(p[2]) || !std::isfinite(q[0]) || !std::isfinite(q[1]) ||
              !std::isfinite(q[2]) || !std::isfinite(q[3])) {
            continue;
          }
          auto& w = server_.world();
          w.write(found->second, integration::GroundPosition, core::Vec3{p[0], p[1], p[2]});
          w.write(found->second, integration::GroundRotation, core::Quat{q[0], q[1], q[2], q[3]});
          w.write(found->second, integration::GroundResting, true);
        } else if (op == "groundItemDestroy") {
          auto found = ground_items_.find(std::uint32_t(number(e, "id")));
          if (found == ground_items_.end()) {
            std::cerr << "RESOURCE ground item destroy: unknown " << number(e, "id") << std::endl;
            continue;
          }
          std::cout << "GROUND stack destroy script=" << found->first
                    << " network=" << found->second << std::endl;
          server_.despawn(found->second);
          ground_items_.erase(found);
        } else if (op == "horseDestroy" || op == "horseOwner" || op == "horseWorld") {
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          if (found == horses_.end())
            continue;
          // Report the horse's authoritative state again, also after a rejected request.
          horse_reports_.forget(found->first);
          if (op == "horseDestroy") {
            server_.despawn(found->second);
            horses_.erase(found);
          } else {
            auto value = std::int64_t(number(e, op == "horseOwner" ? "ownerId" : "dimension"));
            if (op == "horseWorld" && (value < 0 || value > 0xffffffffLL ||
                                       !server_.set_dimension(found->second, std::uint32_t(value))))
              continue;
            server_.world().write(found->second, op == "horseOwner" ? 5 : 6, value);
            if (op == "horseOwner")
              server_.initialize_horse(found->second, core::EntityId(value));
          }
        } else if (op == "horseGear") {
          // Horse.setTack: every body of the horse wears it.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          std::string gear;
          if (found != horses_.end() && outfit_of_pieces(e, gear)) {
            server_.world().write(found->second, integration::HorseGear, gear);
          }
        } else if (op == "horseBuffs") {
          // Horse.addBuff, removeBuff and clearBuffs: every body of the horse carries
          // the list, { id, buffs: [GUID text] }.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          const auto* buffs = field(e, "buffs");
          if (found == horses_.end() || !buffs || buffs->tag() != sdk::Tag::Array ||
              buffs->as_array().size() > integration::kMaxHorseBuffs) {
            continue;
          }
          std::vector<integration::Guid> list;
          for (const Value& item : buffs->as_array()) {
            integration::Guid id{};
            if (item.tag() == sdk::Tag::Str &&
                integration::parse_guid(std::string(item.as_str()), id)) {
              list.push_back(id);
            }
          }
          if (list.size() == buffs->as_array().size()) {
            server_.world().write(found->second, integration::HorseBuffs,
                                  integration::horse_buffs_field(list));
          }
        } else if (op == "horseName") {
          // Horse.name = value: every client's copy of the horse takes the name.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          if (found != horses_.end()) {
            server_.world().write(found->second, 3, text(e, "name"));
          }
        } else if (op == "horseLook") {
          // Horse.setLook: every client builds the horse anew from it.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          const std::string look = text(e, "look");
          if (found != horses_.end() && integration::valid_component_names(look, 4)) {
            server_.world().write(found->second, integration::HorseLook, look);
          }
        } else if (op == "horseMaxCarryWeight") {
          // Horse.maxCarryWeight: every client's saddlebags carry at most that much.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          const double weight = number(e, "weight", -1);
          if (found != horses_.end() && std::isfinite(weight) && weight <= 100000) {
            server_.world().write(found->second, integration::HorseMaxCarryWeight,
                                  float(weight < 0 ? -1 : weight));
          }
        } else if (op == "horseStats") {
          // Horse.maxStamina and Horse.speed: every client's horse soul answers them.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          const double stamina = number(e, "maxStamina", -1);
          const double speed = number(e, "speed", -1);
          if (found != horses_.end() && std::isfinite(stamina) && std::isfinite(speed) &&
              stamina <= 10000 && speed <= 60) {
            server_.world().write(found->second, integration::HorseMaxStamina,
                                  float(stamina <= 0 ? -1 : stamina));
            server_.world().write(found->second, integration::HorseSpeed,
                                  float(speed <= 0 ? -1 : speed));
          }
        } else if (op == "horseKnockOut") {
          // Horse.knockOut: whether a lethal blow knocks the horse out, not kills it.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          if (found != horses_.end()) {
            const bool knock_out = flag_of(e, "knockOut");
            server_.world().write(found->second, integration::HorseKnockOut, knock_out);
          }
        } else if (op == "horseHealth") {
          // Horse.health, kill, knockDown, wake and revive: the resource host's ledger
          // of the horse's health and state (0 alive, 1 knocked out, 2 dead), written as it is.
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          if (found != horses_.end()) {
            server_.set_horse_health(found->second, float(number(e, "health")),
                                     std::int64_t(number(e, "state", -1)));
          }
        } else if (op == "horseIntent") {
          // Horse.moveTo, patrol and hold: the order every client keeps (horse.hpp HorseIntent).
          auto found = horses_.find(std::uint32_t(number(e, "id")));
          const std::string intent = text(e, "intent");
          if (found != horses_.end() && integration::valid_npc_intent(intent)) {
            server_.world().write(found->second, integration::HorseIntent, intent);
          }
        } else if (op == "entityPosition") {
          // Entity.position assignment from a resource: an authoritative teleport of a connected
          // player's avatar through the scoped, epoch-fenced force_transform path.
          const auto id = core::EntityId(number(e, "id"));
          auto pv = field(e, "position");
          if (!pv)
            continue;
          core::Vec3 p{float(number(*pv, "x")), float(number(*pv, "y")), float(number(*pv, "z"))};
          if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
              std::abs(p.x) > 1e6f || std::abs(p.y) > 1e6f || std::abs(p.z) > 1e6f)
            continue;
          if (!connected_avatar(id) && !resource_horse(id)) {
            std::cerr << "RESOURCE position rejected: " << id << " is not a player or horse"
                      << std::endl;
            continue;
          }
          // The resource host already shows the requested value; report the outcome either way.
          forget_reported_entity(id);
          if (!server_.force_transform(id, p)) {
            std::cerr << "RESOURCE position rejected by server: " << id << std::endl;
            continue;
          }
          std::cout << "ENTITY forced position entity=" << id << " pos=" << p.x << "," << p.y << ","
                    << p.z << std::endl;
        } else if (op == "entityRotation") {
          // Entity.rotation assignment from a resource: the JS host already turned Euler degrees
          // into a quaternion. force_rotation rejects anything that is not a finite unit
          // quaternion.
          const auto id = core::EntityId(number(e, "id"));
          auto rv = field(e, "rotation");
          if (!rv)
            continue;
          core::Quat q;
          q.x = float(number(*rv, "x"));
          q.y = float(number(*rv, "y"));
          q.z = float(number(*rv, "z"));
          q.w = float(number(*rv, "w"));
          if (!connected_avatar(id) && !resource_horse(id)) {
            std::cerr << "RESOURCE rotation rejected: " << id << " is not a player or horse"
                      << std::endl;
            continue;
          }
          forget_reported_entity(id);
          if (!server_.force_rotation(id, q)) {
            std::cerr << "RESOURCE rotation rejected by server: " << id << std::endl;
            continue;
          }
          std::cout << "ENTITY forced rotation entity=" << id << " rot=" << q.x << "," << q.y << ","
                    << q.z << "," << q.w << std::endl;
        } else if (op == "entityWorld") {
          // Entity.setDimension from a resource: move a connected player's avatar into another
          // dimension. Snapshot interest and the SDK entity views follow the native dimension.
          const double raw_id = number(e, "id");
          const double request = number(e, "request");
          const double world = number(e, "dimension");
          if (!exact_uint32(raw_id) || !exact_uint32(request) || request < 1 ||
              !connected_avatar(core::EntityId(raw_id))) {
            std::cerr << "RESOURCE dimension rejected: not a connected player" << std::endl;
            continue;
          }
          const auto id = core::EntityId(raw_id);
          forget_reported_entity(id);
          if (!exact_uint32(world) || !server_.set_dimension(id, std::uint32_t(world))) {
            std::cerr << "RESOURCE dimension rejected for player=" << id << std::endl;
          } else {
            std::cout << "ENTITY dimension player=" << id << " world=" << std::uint32_t(world)
                      << std::endl;
            resend_level_edits(id);
          }
          // Always answer with the authoritative dimension so the resource host can settle the
          // pending request, including after a rejection.
          scripts_->send(Value(
              Value::Map{{"op", Value("entityWorldResult")},
                         {"id", Value(std::int64_t(id))},
                         {"request", Value(std::int64_t(request))},
                         {"dimension", Value(std::int64_t(server_.world().slot(id)->dimension))}}));
        } else if (op == "playerMap") {
          // A player goes to another of the server's maps: { id, map, dimension }, the map's
          // index in the server's maps and the player's dimension value there. Their game is
          // given that map's world config under the next revision and loads its level; their
          // avatar moves to that map at once, so nobody on the old map sees them any more.
          const double raw_id = number(e, "id");
          const double raw_map = number(e, "map", -1);
          const double world = number(e, "dimension", double(replication::kGlobalWorld));
          Member* member =
              exact_uint32(raw_id) ? member_with_avatar(core::EntityId(raw_id)) : nullptr;
          if (!member || !exact_uint32(raw_map) || raw_map >= double(maps_.size()) ||
              !exact_uint32(world) ||
              replication::map_of(std::uint32_t(world)) != std::uint32_t(raw_map)) {
            std::cerr << "RESOURCE map change rejected: " << raw_id << " to " << raw_map
                      << std::endl;
            continue;
          }
          const auto map = std::uint32_t(raw_map);
          member->map = map;
          member->world_revision += 1;
          server_.configure_world(member->peer,
                                  maps::world_on_map(config_, maps_[map], member->world_revision));
          forget_reported_entity(member->avatar);
          server_.set_dimension(member->avatar, std::uint32_t(world));
          std::cout << "MAP player=" << member->avatar << " map=" << maps_[map]
                    << " revision=" << member->world_revision << std::endl;
        } else if (op == "entityVisibleTo") {
          // Entity.showOnlyTo from a resource: restrict a connected player's avatar to the
          // connection of another connected player, or clear the restriction with zero.
          const double raw_id = number(e, "id");
          const double raw_target = number(e, "playerId");
          // A ground item (GroundItem.showOnlyTo) goes by its script id.
          const auto ground = exact_uint32(raw_id) ? ground_items_.find(std::uint32_t(raw_id))
                                                   : ground_items_.end();
          const bool ground_item = ground != ground_items_.end();
          if (!exact_uint32(raw_id) || !exact_uint32(raw_target) ||
              (!connected_avatar(core::EntityId(raw_id)) &&
               !resource_horse(core::EntityId(raw_id)) && !resource_npc(core::EntityId(raw_id)) &&
               !ground_item)) {
            std::cerr << "RESOURCE visibility rejected: not a player, horse, NPC or ground item"
                      << std::endl;
            continue;
          }
          const auto id = ground_item ? ground->second : core::EntityId(raw_id);
          const auto target = core::EntityId(raw_target);
          if (target != core::kInvalidEntity && !connected_avatar(target)) {
            std::cerr << "RESOURCE visibility rejected: target " << target
                      << " is not a connected player" << std::endl;
            continue;
          }
          if (!server_.set_visible_to(id, target)) {
            std::cerr << "RESOURCE visibility rejected by server: " << id << std::endl;
            continue;
          }
          std::cout << "ENTITY visible to entity=" << id << " target=" << target << std::endl;
        }
      } catch (const ScriptInboxOverflow&) {
        throw;  // A broken resource command stream must terminate the session, not skip a command.
      } catch (const std::exception& error) {
        std::cerr << "RESOURCE bridge rejected: " << error.what() << std::endl;
      }
    if (!scripts_->alive()) {
      for (const auto& [script, id] : horses_) {
        (void)script;
        server_.despawn(id);
      }
      horses_.clear();
      horse_reports_.clear();
      for (const auto& [script, id] : ground_items_) {
        (void)script;
        server_.despawn(id);
      }
      ground_items_.clear();
      for (const auto& [script, id] : npcs_) {
        (void)script;
        server_.despawn(id);
      }
      npcs_.clear();
      npc_reports_.clear();
      scripts_.reset();
      std::cerr << "RESOURCE HOST exited; resource horses, NPCs and ground items removed"
                << std::endl;
    }
  }
  std::unique_ptr<ScriptProcess> scripts_;
  std::optional<StopRequest> stop_request_;
  // What the resource host already holds; reports carry only changes.
  resources::ViewReportTracker view_reports_;
  resources::ReportCache<core::EntityId, PlayerReport> player_reports_;
  // Each player's latest progression report.
  std::unordered_map<core::EntityId, integration::Progression> progressions_;
  // Players whose progression the current resource host has heard.
  std::unordered_set<core::EntityId> progression_reported_;
  resources::ReportCache<std::uint32_t, HorseReport> horse_reports_;
  unsigned next_report_slice_ = 0;
  std::unordered_map<std::uint32_t, core::EntityId> horses_;
  // NPCs resources spawned (Npc), by script id, and their last reports.
  std::unordered_map<std::uint32_t, core::EntityId> npcs_;
  resources::ReportCache<std::uint32_t, NpcReport> npc_reports_;
  // Stacks lying in the world (GroundItem), by script id.
  std::unordered_map<std::uint32_t, core::EntityId> ground_items_;
  // Containers the server holds (Stash), by script id.
  std::unordered_map<std::uint32_t, core::EntityId> stashes_;
  // The live level edits (LevelEdit) and their dimensions, by edit id, told again
  // to a player who changes world.
  std::map<std::uint32_t, std::pair<integration::LevelEdit, std::uint32_t>> level_edits_;
  std::unordered_map<std::uint64_t, PendingAdmission> admissions_;
  std::unordered_map<std::uint64_t, QueueEntry> queue_;
  // When each address of another build last heard this server's version.
  std::unordered_map<std::uint64_t, double> other_build_answered_;
  std::uint32_t next_queue_id_ = 0;
  std::unordered_map<std::uint64_t, HeardIdentity> identities_;
  std::uint32_t next_admission_request_ = 0;
  struct Member {
    std::unique_ptr<QueueLink> link;
    std::unique_ptr<transport::FragmentingLink> frames;  // over `link`; the server's view of it
    core::EntityId avatar{};
    std::uint16_t player_index = integration::PlayerSlots::kUnassigned;  // Player.playerIndex
    core::PeerId peer{};
    unsigned report_slice = 0;  // tick of the ~100 ms report cycle that reports this member
    double last{};
    metrics::TrafficCounters received;  // valid KSTP packets routed to this member
    // The pair the gate verified when it admitted this address; a disconnect notice must match it.
    std::optional<transport::SessionCredentials> session;
    // Which of the server's maps the player is on, and the revision of the world config their
    // game was last given: a trip to another map sends the next one.
    std::uint32_t map = 0;
    std::uint32_t world_revision = 0;
    // The server view and ownership revisions behind the last complete view report; both the
    // same again means the report would be the same, so it is skipped. Zero: report again.
    std::uint64_t reported_view_revision = 0;
    std::uint64_t reported_ownership = 0;
    core::EntityId reported_avatar{};
  };
  using MemberIterator = std::unordered_map<std::uint64_t, Member>::iterator;
  // The connected member whose avatar `id` is, or null.
  Member* member_with_avatar(core::EntityId id) {
    if (id == core::kInvalidEntity)
      return nullptr;
    for (auto& [address, member] : members_) {
      (void)address;
      if (member.avatar == id)
        return &member;
    }
    return nullptr;
  }
  // Host side of the shared departure order (integration/departure.hpp).
  class MemberDepartureSteps : public integration::DepartureSteps {
   public:
    MemberDepartureSteps(Host& host, std::uint64_t address, const Member& member)
        : host_(host), address_(address), member_(member) {
    }
    void notify_resources() override {
      host_.view_reports_.forget_peer(member_.peer);
      host_.player_reports_.forget(member_.avatar);
      host_.progressions_.erase(member_.avatar);
      host_.progression_reported_.erase(member_.avatar);
      if (host_.scripts_)
        host_.scripts_->send(
            sdk::Value(sdk::Value::Map{{"op", sdk::Value("disconnect")},
                                       {"id", sdk::Value(std::int64_t(member_.avatar))},
                                       {"peer", sdk::Value(std::int64_t(member_.peer))}}));
    }
    void despawn_avatar() override {
      host_.server_.despawn(member_.avatar);
      host_.player_slots_.release(member_.avatar);
    }
    void remove_client() override {
      host_.server_.remove_client(member_.peer);
    }
    void forget_address() override {
      host_.gate_.forget(address_);
    }
    void log_leave(integration::DepartureReason reason) override {
      std::cout << "LEAVE peer=" << member_.peer
                << " reason=" << integration::departure_reason_name(reason) << std::endl;
    }

   private:
    Host& host_;
    std::uint64_t address_;
    const Member& member_;
  };
  bool accepts_disconnect_notice(const Member& member, const Datagram& frame) const {
    if (!member.session)
      return false;
    const auto notice = transport::decode_disconnect_notice(frame);
    return notice && transport::notice_matches_session(*notice, *member.session);
  }
  // Removes one member through the shared departure order. Returns the next member.
  MemberIterator depart(MemberIterator it, integration::DepartureReason reason) {
    MemberDepartureSteps steps(*this, it->first, it->second);
    integration::run_departure(steps, reason);
    departed_fragmented_datagrams_ += it->second.frames->fragmented_datagrams();
    departed_fragments_ += it->second.frames->fragments_sent();
    if (reason == integration::DepartureReason::Disconnect) {
      departures_.disconnect += 1;
    } else if (reason == integration::DepartureReason::Kicked) {
      departures_.kicked += 1;
    } else {
      departures_.timeout += 1;
    }
    kicks_.erase(it->second.avatar);
    counters_.leaves += 1;
    return members_.erase(it);
  }
  replication::SchemaRegistry schema_;
  UdpSocket socket_;
  transport::ConnectionGate gate_;
  transport::RefusalBudget refusal_budget_;
  config::HostSettings settings_;
  integration::Server server_;
  integration::WorldConfig config_;
  // The maps this server runs, config_.map first (maps/server_maps.hpp).
  std::vector<std::string> maps_;
  std::unordered_map<std::uint64_t, Member> members_;
  std::uint64_t departed_fragmented_datagrams_ = 0;
  std::uint64_t departed_fragments_ = 0;
  integration::PlayerSlots player_slots_;
  // The unused rest of the player id block: the next player's id and how many follow it.
  core::EntityId next_player_id_ = core::kInvalidEntity;
  std::uint32_t player_ids_left_ = 0;
  metrics::HostCounters counters_;
  // The resource host's voice orders, outside the long chain of orders: the compiler limits how
  // deep an else-if chain goes. True when it was one of them.
  bool voice_op(const std::string& op, const Value& e) {
    if (op == "voiceEnabled") {
      voice_.set_enabled(flag(e, "enabled", true));
      return true;
    }
    if (op == "voiceIndicator") {
      voice_.set_indicator(flag(e, "shown", true));
      return true;
    }
    if (op == "voiceRange") {
      const auto tier = static_cast<std::uint8_t>(number(e, "tier"));
      if (tier < integration::kVoiceTierCount) {
        voice_.set_range(static_cast<voice::Tier>(tier), float(number(e, "meters")));
      }
      return true;
    }
    if (op == "voiceTier") {
      const auto tier = static_cast<std::uint8_t>(number(e, "tier"));
      if (tier < integration::kVoiceTierCount) {
        voice_.set_tier(core::EntityId(number(e, "id")), tier);
      }
      return true;
    }
    if (op == "voiceMuted") {
      voice_.set_muted(core::EntityId(number(e, "id")), flag(e, "muted", false));
      return true;
    }
    if (op == "voiceMuffle") {
      const double amount = std::clamp(number(e, "amount"), 0.0, 1.0);
      voice_.set_muffle(core::EntityId(number(e, "id")), std::uint8_t(std::lround(amount * 255.0)));
      return true;
    }
    if (op == "voiceDeaf") {
      voice_.set_deaf(core::EntityId(number(e, "id")), flag(e, "deaf", false));
      return true;
    }
    if (op == "voiceListenerMute") {
      voice_.set_listener_mute(core::EntityId(number(e, "id")), core::EntityId(number(e, "talker")),
                               flag(e, "muted", false));
      return true;
    }
    return false;
  }
  // Each 100 ms: the relay learns who plays where; the resource host hears who talks.
  void tick_voice(double now) {
    voice_.pump_inline();
    for (const auto& [player, key] : server_.take_voice_keys()) {
      voice_.player_key(player, key);
    }
    for (const auto& [player, tier] : server_.take_voice_tier_requests()) {
      if (scripts_) {
        scripts_->send(Value(Value::Map{{"op", Value("voiceTierRequest")},
                                        {"id", Value(std::int64_t(player))},
                                        {"tier", Value(std::int64_t(tier))}}));
      }
    }
    for (const auto& change : voice_.take_talk_changes()) {
      if (scripts_) {
        scripts_->send(Value(Value::Map{{"op", Value("voiceTalking")},
                                        {"id", Value(std::int64_t(change.player))},
                                        {"talking", Value(change.talking)}}));
      }
    }
    if (now - voice_told_at_ < 0.1) {
      return;
    }
    voice_told_at_ = now;
    voice_.forget_gone_players(
        [this](core::EntityId player) { return server_.world().slot(player) != nullptr; });
    for (const auto& [address, member] : members_) {
      (void)address;
      if (!server_.peer_world_ready(member.peer)) {
        continue;
      }
      voice_.player_ready(member.avatar);
      const auto position = server_.world().read(member.avatar, Position);
      const auto* slot = server_.world().slot(member.avatar);
      if (position && slot) {
        voice_.player_at(member.avatar, std::get<core::Vec3>(*position), slot->dimension);
      }
    }
  }
  double voice_told_at_ = -1.0;
  VoiceHost voice_{[this](const voice::Address& to, const std::vector<std::uint8_t>& data) {
                     sockaddr_in address{};
                     address.sin_family = AF_INET;
                     address.sin_addr.s_addr = to.ip;
                     address.sin_port = to.port;
                     socket_.try_send(address, data);
                   },
                   [this](core::EntityId player, const integration::VoiceState& state) {
                     return server_.send_voice_state(player, state);
                   }};
  bool phase_timing_ = false;
  HostPhaseTimes phase_times_;
  // The tick's work besides the snapshot builds, averaged (seconds; below 0 before the first).
  double other_tick_seconds_ = -1.0;
  double inactivity_timeout_ = kDefaultInactivityTimeout;
  double joining_inactivity_timeout_ = kDefaultJoiningInactivityTimeout;
  HostDepartureCounts departures_;
  // Players a resource kicked: avatar -> when they are dropped (their client heard why first).
  std::unordered_map<core::EntityId, double> kicks_;
  // How long a kicked player stays connected for its SESSION_END to arrive, in seconds.
  static constexpr double kKickDropDelaySeconds = 0.5;
};
class Client {
 public:
  // `local`: the address its socket binds (any port); a tool talking to a server on this machine
  // binds 127.0.0.1, which no firewall asks about.
  Client(const char* ip, unsigned short port, const char* local = "0.0.0.0")
      : schema_(player_schema()),
        socket_(address(local, 0)),
        remote_(address(ip, port)),
        link_(socket_, remote_),
        frames_(&link_, transport::kDefaultMtu, transport::FragmentFraming::Verbatim),
        handshake_(client_nonce_),
        pacer_(client_nonce_),
        peer_(schema_, &frames_) {
    frames_.set_reassembly_timeout(kClientReassemblyTimeoutTicks);
    frames_.set_max_partials(kClientMaxPartials);
    frames_.set_max_message_bytes(kClientMaxMessageBytes);
    pending_ = handshake_.start();
    // Kit tests only: KCDS_TEST_RECEIVE_DELAY_MS holds every datagram of the session this
    // long before the client reads it, as a network delay would; up to one second.
    if (const char* delay = std::getenv("KCDS_TEST_RECEIVE_DELAY_MS")) {
      const int milliseconds = std::atoi(delay);
      if (milliseconds > 0 && milliseconds <= 1000) {
        receive_delay_ = double(milliseconds) / 1000.0;
      }
    }
  }
  // Voice datagrams skip the session: the handler runs on the thread that ticks this client.
  void set_voice_datagram_handler(void (*handler)(const std::uint8_t*, std::size_t)) {
    voice_datagram_handler_ = handler;
  }
  // From any thread: one voice datagram to the server.
  bool send_voice_datagram(const std::uint8_t* data, std::size_t size) {
    const std::uint64_t endpoint = voice_endpoint_.load();
    if (endpoint == 0) {
      return socket_.try_send(remote_, Datagram(data, data + size));
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = static_cast<std::uint32_t>(endpoint >> 16);
    address.sin_port = static_cast<std::uint16_t>(endpoint & 0xFFFF);
    return socket_.try_send(address, Datagram(data, data + size));
  }
  // Where voice goes, in network order; 0 for the game server itself.
  void set_voice_endpoint(std::uint32_t ip, std::uint16_t port) {
    voice_endpoint_.store(ip == 0 ? 0 : (std::uint64_t(ip) << 16) | port);
  }
  // Graceful leave (KSTP DISCONNECT_NOTICE, transport/disconnect.hpp). Marks this client
  // disconnected, so every later tick() does nothing and no packet follows the notice, then sends
  // transport::kDisconnectNoticeCopies back-to-back copies of the notice that was encoded when the
  // handshake produced the session. Returns true when at least one copy was handed to the OS;
  // false when no session exists yet (nothing to tell the server), when every send failed, or
  // when disconnect() already ran. Idempotent; never throws. The client cannot reconnect
  // afterwards: create a new Client for a new session.
  //
  // Constraints for the caller (game DLL shutdown):
  // - Any thread. It reads only state that is immutable once the notice is ready (socket handle,
  //   server address, encoded notice) and two atomics, so it may run while another thread is
  //   inside tick(); that tick may still send at most one more datagram, which the server ignores
  //   once the member is gone. Calling it with the tick thread's lock held, or after that thread
  //   stopped, avoids even that.
  // - It takes no lock, waits for nothing, joins nothing, does not allocate, and issues at most
  //   kDisconnectNoticeCopies non-blocking sendto calls on the already-open socket.
  // - Process exit (DLL_PROCESS_DETACH with lpReserved != nullptr: loader lock held, other
  //   threads already terminated): call it directly and do not lock a mutex first, because a
  //   terminated thread may still own it. Winsock must still be initialized (no WSACleanup
  //   before it). Winsock calls under the loader lock are best effort; prefer an earlier engine
  //   shutdown hook when one exists. The server's inactivity timeout remains the fallback.
  // - It does not stop the resource runtime; ~Client does that, and it may wait, so never run the
  //   destructor under the loader lock.
  bool disconnect() noexcept {
    if (disconnected_.exchange(true))
      return false;
    if (!disconnect_notice_ready_.load(std::memory_order_acquire))
      return false;
    bool sent = false;
    for (unsigned copy = 0; copy < transport::kDisconnectNoticeCopies; ++copy) {
      if (socket_.try_send(remote_, disconnect_notice_))
        sent = true;
    }
    return sent;
  }
  bool disconnected() const noexcept {
    return disconnected_.load();
  }
  // The admitted session's handshake pair, once the CHALLENGE was answered (diagnostics, tests).
  std::optional<transport::SessionCredentials> session_credentials() const {
    return session_;
  }
  // No server at all (World Architect's local mode, Client/runtime/architect/local_mode.hpp):
  // tick() sends and reads nothing and only drives the resource runtime.
  void set_offline() noexcept {
    offline_ = true;
  }
  bool offline() const noexcept {
    return offline_;
  }
  void tick(double now) {
    if (disconnected_.load())
      return;
    if (offline_) {
      tick_resource_runtime(now);
      return;
    }
    if (!handshake_.established() && pacer_.due(now)) {
      if (handshake_.refused()) {
        // A retry needs a new platform proof even when the server repeats its nonce.
        licence_completed_ = false;
        pending_ = handshake_.start();  // the refusal delay elapsed: begin a new attempt
      }
      if (licence_ready()) {
        send_identity();
        socket_.send(remote_, pending_);
        pacer_.sent(now);
      }
    }
    sockaddr_in from{};
    Datagram d;
    for (unsigned budget = 0; budget < 1024 && socket_.recv(from, d); ++budget) {
      if (key(from) != key(remote_)) {
        // A standalone voice server sends from its own address.
        const std::uint64_t endpoint = voice_endpoint_.load();
        const std::uint64_t sender =
            (std::uint64_t(from.sin_addr.s_addr) << 16) | std::uint64_t(from.sin_port);
        if (endpoint != 0 && sender == endpoint && voice_datagram_handler_ &&
            voice::is_voice_datagram(d.data(), d.size())) {
          voice_datagram_handler_(d.data(), d.size());
        }
        continue;
      }
      if (voice::is_voice_datagram(d.data(), d.size())) {
        if (voice_datagram_handler_) {
          voice_datagram_handler_(d.data(), d.size());
        }
        continue;
      }
      last_server_packet_ = now;
      if (!handshake_.established()) {
        const bool was_refused = handshake_.refused();
        Datagram reply;
        if (handshake_.on_datagram(d, reply)) {
          prepare_disconnect_notice(reply);
          pending_ = reply;
          pacer_.progressed();
          if (licence_ready()) {
            send_identity();
            socket_.send(remote_, reply);
            pacer_.sent(now);
          }
        } else if (handshake_.refused() && !was_refused) {
          // Server full: back off as the server asked instead of retrying every 0.25 s.
          refusals_received_ += 1;
          pacer_.refused(now, handshake_.refusal().retry_after_ms);
        }
      } else if (d.size() > 1 && receive_delay_ > 0 && delayed_.size() < 4096) {
        delayed_.push_back({now + receive_delay_, std::move(d)});
      } else if (d.size() > 1 && receive_delay_ <= 0 && link_.inbox.size() < 256)
        link_.inbox.push_back(std::move(d));
    }
    while (!delayed_.empty() && delayed_.front().first <= now && link_.inbox.size() < 256) {
      link_.inbox.push_back(std::move(delayed_.front().second));
      delayed_.pop_front();
    }
    if (handshake_.established()) {
      peer_.tick(now);
      frames_.poll();  // expire incomplete fragment sets
    }
    tick_resource_runtime(now);
  }
  ~Client() {
    if (!resource_runtime_ || !resource_runtime_->alive() || !runtime_ready_)
      return;
    resource_runtime_->send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("disconnect")}}));
    const auto deadline = clock_seconds() + 10.5;
    while (resource_runtime_->alive() && clock_seconds() < deadline) {
      for (const auto& message : resource_runtime_->take()) {
        if (integration::script_operation(message) == "runtimeStopped")
          return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  // The disk cache of downloaded resource files; without one every join downloads
  // every file.
  static std::string text_of(const sdk::Value& value, const char* name) {
    const auto* field = integration::script_field(value, name);
    return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
  }

  static std::int64_t whole_of(const sdk::Value& value, const char* name) {
    const auto* field = integration::script_field(value, name);
    if (field && field->tag() == sdk::Tag::Int) {
      return field->as_int();
    }
    if (field && field->tag() == sdk::Tag::Double) {
      return std::int64_t(field->as_double());
    }
    return -1;
  }

  // The server's catalog (catalogInfo, then its catalogData chunks), assembled here too: once its
  // SHA-256 is the catalogHash the server named, the hashes it lists are the only files this
  // session may take from the cache. The runtime's scripts cannot change it:
  // it comes from the server's connection.
  void start_catalog(const sdk::Value& info) {
    catalog_revision_ = whole_of(info, "revision");
    catalog_hash_ = text_of(info, "catalogHash");
    const std::int64_t size = whole_of(info, "size");
    catalog_bytes_.clear();
    catalog_received_ = 0;
    catalog_hashes_.clear();
    if (size > 0 && size <= kMaxCatalogBytes) {
      catalog_bytes_.assign(std::size_t(size), '\0');
    }
    if (catalog_listener_) {
      catalog_listener_(catalog_hashes_);
    }
  }

  void keep_catalog(const sdk::Value& data) {
    const std::int64_t offset = whole_of(data, "offset");
    if (catalog_bytes_.empty() || whole_of(data, "revision") != catalog_revision_ || offset < 0 ||
        !download_cache_) {
      return;
    }
    const std::optional<std::string> piece = decode_base64(text_of(data, "data"));
    if (!piece || std::size_t(offset) + piece->size() > catalog_bytes_.size()) {
      return;
    }
    std::copy(piece->begin(), piece->end(), catalog_bytes_.begin() + std::ptrdiff_t(offset));
    catalog_received_ += std::int64_t(piece->size());
    if (catalog_received_ < std::int64_t(catalog_bytes_.size())) {
      return;
    }
    if (download_cache_->digest(catalog_bytes_) != catalog_hash_) {
      resource_log("catalog does not match its hash: no cached files this session");
      catalog_bytes_.clear();
      return;
    }
    // Every "hash":"<64 hex>" of the catalog: its files' (and its resources' own).
    const std::string key = "\"hash\":\"";
    for (std::size_t at = catalog_bytes_.find(key); at != std::string::npos;
         at = catalog_bytes_.find(key, at + key.size())) {
      const std::string hash = catalog_bytes_.substr(at + key.size(), 64);
      if (hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos) {
        catalog_hashes_.insert(hash);
      }
    }
    catalog_bytes_.clear();
    resource_log("catalog verified: " + std::to_string(catalog_hashes_.size()) + " hashes");
    if (catalog_listener_) {
      catalog_listener_(catalog_hashes_);
    }
  }

  bool in_catalog(const std::string& hash) const {
    return catalog_hashes_.count(hash) != 0;
  }

  static std::optional<std::string> decode_base64(const std::string& text) {
    if (text.size() % 4 != 0) {
      return std::nullopt;
    }
    std::string out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t quad = 0;
    int count = 0;
    int padding = 0;
    for (const char c : text) {
      int digit = -1;
      if (c >= 'A' && c <= 'Z') {
        digit = c - 'A';
      } else if (c >= 'a' && c <= 'z') {
        digit = c - 'a' + 26;
      } else if (c >= '0' && c <= '9') {
        digit = c - '0' + 52;
      } else if (c == '+') {
        digit = 62;
      } else if (c == '/') {
        digit = 63;
      } else if (c == '=') {
        digit = 0;
        ++padding;
      } else {
        return std::nullopt;
      }
      quad = (quad << 6) | std::uint32_t(digit);
      if (++count == 4) {
        out.push_back(char((quad >> 16) & 255));
        out.push_back(char((quad >> 8) & 255));
        out.push_back(char(quad & 255));
        quad = 0;
        count = 0;
      }
    }
    if (padding > 2) {
      return std::nullopt;
    }
    out.resize(out.size() - std::size_t(padding));
    return out;
  }

  // { op: "fileEnsure", hash, requestId }: { op: "fileReady", hash, ok, requestId } once the
  // file of this server's catalog is verified in the cache, downloaded over HTTP when it is not.
  void ensure_file(const sdk::Value& request) {
    const std::string hash = text_of(request, "hash");
    const auto* request_id = integration::script_field(request, "requestId");
    if (!request_id) {
      return;
    }
    const auto ready = [&](bool ok) {
      if (resource_runtime_) {
        resource_runtime_->send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("fileReady")},
                                                           {"hash", sdk::Value(hash)},
                                                           {"ok", sdk::Value(ok)},
                                                           {"requestId", *request_id}}));
      }
    };
    if (!download_cache_ || !in_catalog(hash)) {
      ready(false);
      return;
    }
    if (download_cache_->ensure(hash)) {
      ready(true);
      return;
    }
    if (!http_files_ || !http_files_offered_ || http_failed_.count(hash)) {
      ready(false);
      return;
    }
    auto waiting = http_ensures_.find(hash);
    if (waiting == http_ensures_.end()) {
      waiting = http_ensures_.emplace(hash, std::vector<sdk::Value>{}).first;
      if (!http_waiting_.count(hash)) {
        http_files_->start(hash);
      }
    }
    waiting->second.push_back(*request_id);
  }

  // A chunk request of a file the cache lacks, when the server offers its files over HTTP: the
  // file is downloaded whole (once) and the request answered from the cache when it is there.
  // False when the HTTP way is not open for it (the request goes to the server).
  bool request_over_http(const sdk::Value& request) {
    const auto* hash_field = integration::script_field(request, "hash");
    if (!http_files_ || !http_files_offered_ || !hash_field || hash_field->tag() != sdk::Tag::Str) {
      return false;
    }
    const std::string hash = hash_field->as_str();
    if (http_failed_.count(hash)) {
      return false;
    }
    auto waiting = http_waiting_.find(hash);
    if (waiting == http_waiting_.end()) {
      waiting = http_waiting_.emplace(hash, std::vector<sdk::Value>{}).first;
      if (!http_ensures_.count(hash)) {
        http_files_->start(hash);
      }
    }
    waiting->second.push_back(request);
    return true;
  }

  // The HTTP files that came: their waiting requests answered from the cache, or sent to the
  // server when the file did not come or was not right.
  void finish_http_files() {
    if (!http_files_) {
      return;
    }
    for (resources::HttpFile& file : http_files_->take_done()) {
      const bool kept =
          file.ok && download_cache_ && download_cache_->put_file(file.hash, file.path);
      if (!kept && !file.path.empty()) {
        std::error_code error;
        std::filesystem::remove(file.path, error);
      }
      if (kept) {
        ++http_files_done_;
      } else {
        http_failed_.insert(file.hash);
        resource_log("HTTP file " + file.hash.substr(0, 12) + " did not come: over the connection");
      }
      if (auto ensures = http_ensures_.find(file.hash); ensures != http_ensures_.end()) {
        for (const sdk::Value& request_id : ensures->second) {
          if (resource_runtime_) {
            resource_runtime_->send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("fileReady")},
                                                               {"hash", sdk::Value(file.hash)},
                                                               {"ok", sdk::Value(kept)},
                                                               {"requestId", request_id}}));
          }
        }
        http_ensures_.erase(ensures);
      }
      auto waiting = http_waiting_.find(file.hash);
      if (waiting == http_waiting_.end()) {
        continue;
      }
      for (const sdk::Value& request : waiting->second) {
        std::optional<sdk::Value> reply;
        if (kept) {
          reply = download_cache_->answer(request);
        }
        if (reply && resource_runtime_) {
          resource_runtime_->send(*reply);
        } else {
          peer_.send_script(request);
        }
      }
      http_waiting_.erase(waiting);
    }
  }

  void set_download_cache(std::unique_ptr<resources::DownloadCache> cache) {
    download_cache_ = std::move(cache);
  }
  // Told the hashes of the server's verified catalog (none until it is): the game's own readers of
  // cached files (native UI, custom maps, pages) take only these.
  void set_catalog_listener(std::function<void(const std::set<std::string>&)> listener) {
    catalog_listener_ = std::move(listener);
  }
  // The server's files over HTTP when its catalog offers them (http_files.hpp).
  void set_http_files(std::unique_ptr<resources::HttpFiles> files) {
    http_files_ = std::move(files);
  }
  void start_resource_runtime(const ProcessText& executable) {
    resource_runtime_executable_ = executable;
    resource_runtime_ = std::make_unique<ScriptProcess>(executable, std::vector<ProcessText>{});
    ++resource_runtime_generation_;
  }
  // Counts the resource runtimes started: one that ended is started anew, and what
  // the game's modules sent the old one goes to the new one again.
  std::uint32_t resource_runtime_generation() const {
    return resource_runtime_generation_;
  }
  bool resources_ready() const {
    return resources_ready_;
  }
  // The resource runtime failed or ended since it started (it is started anew).
  bool resource_runtime_failed() const {
    return runtime_failed_;
  }
  // Download progress of the server's client resources for the connecting screen.
  struct ResourceProgress {
    std::int64_t resources_done = 0;
    std::int64_t resources_total = 0;
    std::int64_t bytes_done = 0;
    std::int64_t bytes_total = 0;
  };
  // The latest report of the resource runtime for the current world, once one arrived.
  std::optional<ResourceProgress> resource_progress() const {
    return resource_progress_;
  }
  void set_resource_logger(std::function<void(const std::string&)> logger) {
    resource_logger_ = std::move(logger);
  }
  // Sees every message of the resource runtime first; returning true keeps it local (it is not
  // forwarded to the server). Runs on the thread that ticks this client.
  void set_local_runtime_handler(std::function<bool(const sdk::Value&)> handler) {
    local_handler_ = std::move(handler);
  }
  // The resource runtime runs and has loaded its resources.
  bool resource_runtime_ready() const {
    return resource_runtime_ && resource_runtime_->alive() && runtime_ready_;
  }
  // Sends a message to the resource runtime; false when it is not running.
  // False until the runtime said it is ready: its page drops what comes before it has loaded, so
  // a report sent then was lost while its sender counted it as heard (the web views' readiness
  // after a join from the main menu). The game's modules send it again.
  bool send_to_resource_runtime(const sdk::Value& message) {
    if (!resource_runtime_ || !resource_runtime_->alive() || !runtime_ready_)
      return false;
    resource_runtime_->send(message);
    return true;
  }
  core::EntityId own_avatar() const {
    for (const auto& [e, v] : peer_.mirror().entities()) {
      if (v.type == core::type_id("NetworkPlayer") && peer_.owns(e))
        return e;
    }
    return core::kInvalidEntity;
  }
  integration::ClientPeer& peer() {
    return peer_;
  }
  const integration::ClientPeer& peer() const {
    return peer_;
  }
  bool established() const {
    return handshake_.established();
  }
  // True while the server has refused this client (server full) and the client waits to retry.
  bool refused() const {
    return handshake_.refused();
  }
  // Why the server refused the last handshake, once a CONNECT_REFUSED arrived.
  std::optional<transport::RefusalReason> last_refusal() const {
    if (refusals_received_ == 0) {
      return std::nullopt;
    }
    return handshake_.refusal().reason;
  }
  // The server's reason for that refusal (connection.reject(reason)), or empty without one.
  std::string last_refusal_text() const {
    return handshake_.refusal_text();
  }
  // Who this client says it is to the server's admission, sent with every response.
  void set_identity(const std::string& nickname, const std::string& steam_id,
                    const std::string& discord_id = {}, const std::string& hardware_id = {}) {
    identity_.nickname = nickname;
    identity_.steam_id = steam_id;
    identity_.discord_id = discord_id;
    identity_.hardware_id = hardware_id;
  }
  // The text the player's launcher gave this join (kcds-launcher --join-token),
  // sent with the identity; the server's resources read it as connection.joinToken.
  void set_join_token(const std::string& token) {
    identity_.join_token = token;
  }
  // The KCDS Master's join pass (the licence gate), sent with the identity.
  void set_licence(const std::string& pass) {
    identity_.licence = pass;
  }
  // Called on a dedicated worker once the server's challenge is known. HTTP and
  // platform waits must not block the socket/simulation tick or the game thread.
  struct LicenceProof {
    std::string text;
    double refresh_seconds = 180;
    // Without a pass: why, for the player: the Master's central ban list or a copy
    // that could not be proven an original one.
    std::string refusal;
    bool banned = false;
    LicenceProof() = default;
    LicenceProof(std::string pass, double refresh = 180, std::string refused = {},
                 bool was_banned = false)
        : text(std::move(pass)),
          refresh_seconds(refresh),
          refusal(std::move(refused)),
          banned(was_banned) {
    }
  };
  void set_licence_provider(std::function<LicenceProof(std::uint64_t)> provider) {
    licence_provider_ = std::move(provider);
  }
  // The server's password: the proof for a handshake's server nonce, sent with the
  // identity; unset or empty sends none.
  void set_password_proof(std::function<std::string(std::uint64_t)> proof) {
    password_proof_ = std::move(proof);
  }
  // Why no join pass could be had for this player, or empty: a central ban or a
  // copy that could not be proven. No server admits them then; the join stops.
  const std::string& licence_refusal() const {
    return licence_refusal_;
  }
  bool licence_refusal_banned() const {
    return licence_refusal_banned_;
  }
  // The server's last word on this client's waiting response: its place in a full server's
  // queue, how long it waited, the resources' line and fields.
  std::optional<transport::WaitStatus> wait_status() const {
    return handshake_.wait_status();
  }
  // True once any datagram of the server arrived (its handshake challenge or a refusal).
  bool heard_from_server() const {
    return last_server_packet_ >= 0;
  }
  // Seconds since the server's last datagram, or a negative value before the first one.
  double seconds_since_server(double now) const {
    return last_server_packet_ < 0 ? -1.0 : now - last_server_packet_;
  }
  // CONNECT_REFUSED replies accepted so far, and the time of the next handshake attempt.
  std::uint64_t refusals_received() const {
    return refusals_received_;
  }
  double next_handshake_attempt() const {
    return pacer_.next_send();
  }
  // Cumulative UDP payload counters of this client's socket, handshake datagrams included.
  const metrics::TrafficCounters& traffic() const {
    return socket_.traffic();
  }
  // Measurement tools only: emulated loss and delay on this client's socket.
  void set_impairment(const netsim::ImpairmentSettings& settings) {
    socket_.set_impairment(settings);
  }
  const UdpSocket& socket() const {
    return socket_;
  }

 private:
  // Encodes the notice once, from the CHALLENGE_RESPONSE this client sends, so disconnect() only
  // has to send an existing buffer.
  void prepare_disconnect_notice(const Datagram& challenge_response) {
    if (disconnect_notice_ready_.load(std::memory_order_relaxed))
      return;
    session_ = transport::session_credentials_from_handshake(challenge_response);
    if (!session_)
      return;
    disconnect_notice_ =
        transport::encode_disconnect_notice(*session_, transport::DisconnectReason::ClientQuit);
    disconnect_notice_ready_.store(true, std::memory_order_release);
  }
  void resource_log(const std::string& text) {
    if (resource_logger_)
      resource_logger_(text);
    else
      std::cout << "CLIENT RESOURCE " << text << std::endl;
  }
  void tick_resource_runtime(double now) {
    if (!resource_runtime_)
      return;
    if (!resource_runtime_->alive()) {
      resources_ready_ = false;
      if (runtime_ended_at_ < 0) {
        resource_log("runtime exited");
        runtime_failed_ = true;
        runtime_ended_at_ = now;
      }
      restart_resource_runtime(now);
      return;
    }
    const auto& config = peer_.world_config();
    if (config && config->revision != resource_world_revision_) {
      if (resource_world_revision_)
        resource_runtime_->send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("disconnect")}}));
      resource_world_revision_ = config->revision;
      resources_ready_ = false;
      catalog_requested_ = false;
      player_ready_sent_ = false;
      resource_progress_.reset();
    }
    for (const auto& value : resource_runtime_->take()) {
      // Messages that stay on this machine (the game client's web views) never reach the server.
      if (local_handler_ && local_handler_(value))
        continue;
      const auto op = integration::script_operation(value);
      if (op == "runtimeReady") {
        runtime_ready_ = true;
        resource_log("runtime ready");
      } else if (op == "runtimeFailed") {
        resources_ready_ = false;
        runtime_failed_ = true;
        const auto* reason = integration::script_field(value, "text");
        resource_log(reason && reason->tag() == sdk::Tag::Str
                         ? "runtime failed: " + reason->as_str()
                         : std::string("runtime failed"));
      } else if (op == "log") {
        const auto* message = integration::script_field(value, "text");
        if (message && message->tag() == sdk::Tag::Str)
          resource_log(message->as_str());
      } else if (op == "resourcesReady") {
        resources_ready_ = true;
        if (download_cache_) {
          resource_log("download cache answered " + std::to_string(download_cache_->hits()) +
                       " requests; " + std::to_string(http_files_done_) + " files over HTTP");
        }
        peer_.send_script(value);
      } else if (op == "fileRequest" && download_cache_) {
        // A file this machine already has, verified: answered without the network,
        // but only a file of this server's catalog, so a server's scripts never read another
        // server's files from this machine's cache.
        const bool ours = in_catalog(text_of(value, "hash"));
        if (auto reply = ours ? download_cache_->answer(value) : std::nullopt) {
          resource_runtime_->send(*reply);
        } else if (!ours || !request_over_http(value)) {
          peer_.send_script(value);
        }
      } else if (op == "fileEnsure") {
        // A big file the runtime leaves on the disk: verified there, or downloaded whole first.
        ensure_file(value);
      } else if (op == "resourceProgress") {
        // Local only: the connecting screen shows it; the server never needs it.
        if (const auto progress = read_resource_progress(value)) {
          if (!resource_progress_ ||
              resource_progress_->resources_done != progress->resources_done) {
            resource_log("progress " + std::to_string(progress->resources_done) + "/" +
                         std::to_string(progress->resources_total) + " bytes " +
                         std::to_string(progress->bytes_done) + "/" +
                         std::to_string(progress->bytes_total));
          }
          resource_progress_ = *progress;
        }
      } else {
        peer_.send_script(value);
      }
    }
    finish_http_files();
    if (!runtime_ready_ || !config || runtime_failed_)
      return;
    if (!catalog_requested_) {
      catalog_requested_ =
          peer_.send_script(sdk::Value(sdk::Value::Map{{"op", sdk::Value("catalogRequest")}}));
      if (catalog_requested_)
        resource_log("catalog requested");
    }
    for (const auto& message : peer_.take_scripts()) {
      // World Architect opens only in the client's local mode, never at a server's word.
      const auto operation = integration::script_operation(message);
      if (operation == "architectOpen" || operation == "architectTest" ||
          operation == "architectPicturesKept" || operation == "architectProject") {
        resource_log("a server sent " + operation + ": dropped");
        continue;
      }
      if (integration::script_operation(message) == "catalogInfo") {
        resources_ready_ = false;
        const auto* http = integration::script_field(message, "httpFiles");
        http_files_offered_ = http && http->tag() == sdk::Tag::Bool && http->as_bool();
        // A broadcast catalogInfo has no token: the one of this game's own reply is kept.
        const std::string token = text_of(message, "httpToken");
        if (http_files_ && !token.empty()) {
          http_files_->set_token(token);
        }
        start_catalog(message);
        resource_log(http_files_offered_ ? "catalog received; files over HTTP"
                                         : "catalog received");
      }
      if (integration::script_operation(message) == "catalogData") {
        keep_catalog(message);
      }
      if (download_cache_ && integration::script_operation(message) == "fileData") {
        download_cache_->keep(message);
      }
      resource_runtime_->send(message);
    }
    // playerReady on the client: the game took its own loading screen down (not world-ready).
    if (resources_ready_ && peer_.gameplay_started() && !player_ready_sent_) {
      player_ready_sent_ =
          resource_runtime_->send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("playerReady")}}));
    }
    if (handshake_.established() && last_server_packet_ >= 0 && now - last_server_packet_ > 10 &&
        resources_ready_) {
      resource_runtime_->send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("disconnect")}}));
      resources_ready_ = false;
    }
  }
  // A resource runtime that ended (its WebView2 went away) is started anew after a
  // short wait, and the new one loads the server's resources as at the join. A few times only: one
  // that cannot run at all stays ended.
  void restart_resource_runtime(double now) {
    constexpr double kRestartDelay = 2.0;
    constexpr int kMaxRestarts = 5;
    if (resource_runtime_restarts_ >= kMaxRestarts || now - runtime_ended_at_ < kRestartDelay ||
        resource_runtime_executable_.empty()) {
      return;
    }
    ++resource_runtime_restarts_;
    start_resource_runtime(resource_runtime_executable_);
    runtime_ready_ = false;
    runtime_failed_ = false;
    runtime_ended_at_ = -1;
    catalog_requested_ = false;
    resources_ready_ = false;
    player_ready_sent_ = false;
    resource_progress_.reset();
    resource_log("runtime started again (" + std::to_string(resource_runtime_restarts_) + " of " +
                 std::to_string(kMaxRestarts) + ")");
  }
  // A resourceProgress report with four whole, non-negative counts and done <= total.
  static std::optional<ResourceProgress> read_resource_progress(const sdk::Value& value) {
    auto count = [&](const char* name) -> std::int64_t {
      const auto* field = integration::script_field(value, name);
      if (!field)
        return -1;
      if (field->tag() == sdk::Tag::Int)
        return field->as_int();
      if (field->tag() == sdk::Tag::Double) {
        const double number = field->as_double();
        const bool whole = number >= 0 && number <= 9.0e15 && number == std::floor(number);
        return whole ? static_cast<std::int64_t>(number) : -1;
      }
      return -1;
    };
    ResourceProgress progress;
    progress.resources_done = count("resourcesDone");
    progress.resources_total = count("resourcesTotal");
    progress.bytes_done = count("bytesDone");
    progress.bytes_total = count("bytesTotal");
    const bool counts = progress.resources_done >= 0 && progress.resources_total >= 0 &&
                        progress.bytes_done >= 0 && progress.bytes_total >= 0;
    if (!counts || progress.resources_done > progress.resources_total ||
        progress.bytes_done > progress.bytes_total)
      return std::nullopt;
    return progress;
  }
  std::function<bool(const sdk::Value&)> local_handler_;
  std::function<void(const std::string&)> resource_logger_;
  std::unique_ptr<ScriptProcess> resource_runtime_;
  ProcessText resource_runtime_executable_;
  std::uint32_t resource_runtime_generation_ = 0;
  int resource_runtime_restarts_ = 0;
  double runtime_ended_at_ = -1;
  std::unique_ptr<resources::DownloadCache> download_cache_;
  // The server's HTTP files (http_files.hpp): whether its catalog offers them, the chunk requests
  // waiting for a file on its way, and the files the HTTP way could not bring this session.
  std::unique_ptr<resources::HttpFiles> http_files_;
  bool http_files_offered_ = false;
  std::map<std::string, std::vector<sdk::Value>> http_waiting_;
  // fileEnsure requests waiting for a file on its way over HTTP, by hash (their requestIds).
  std::map<std::string, std::vector<sdk::Value>> http_ensures_;
  // The server's catalog as it arrives, and the hashes it lists once verified.
  static constexpr std::int64_t kMaxCatalogBytes = 24 * 1024 * 1024;
  std::int64_t catalog_revision_ = -1;
  std::string catalog_hash_;
  std::string catalog_bytes_;
  std::int64_t catalog_received_ = 0;
  std::set<std::string> catalog_hashes_;
  std::function<void(const std::set<std::string>&)> catalog_listener_;
  std::set<std::string> http_failed_;
  std::uint64_t http_files_done_ = 0;
  std::optional<ResourceProgress> resource_progress_;
  bool runtime_ready_ = false;
  bool runtime_failed_ = false;
  bool catalog_requested_ = false;
  bool resources_ready_ = false;
  bool player_ready_sent_ = false;
  std::uint32_t resource_world_revision_ = 0;
  double last_server_packet_ = -1;
  // The kit's simulated network delay (KCDS_TEST_RECEIVE_DELAY_MS, seconds; 0 off) and the
  // datagrams it holds with the time each may be read.
  double receive_delay_ = 0;
  std::deque<std::pair<double, Datagram>> delayed_;
  replication::SchemaRegistry schema_;
  void (*voice_datagram_handler_)(const std::uint8_t*, std::size_t) = nullptr;
  std::atomic<std::uint64_t> voice_endpoint_{0};
  UdpSocket socket_;
  sockaddr_in remote_;
  QueueLink link_;
  // Datagrams above the safe UDP payload are split into verbatim fragments (protocol 2).
  static constexpr std::uint32_t kClientReassemblyTimeoutTicks = 60;
  // Reassembly memory stays below 16 MiB even under forged fragments (the defaults allow 256 MiB).
  // A host datagram is far smaller: snapshots are capped at 32 KiB of credit per tick.
  static constexpr std::size_t kClientMaxPartials = 16;
  static constexpr std::uint32_t kClientMaxMessageBytes = 1024 * 1024;
  transport::FragmentingLink frames_;
  std::uint64_t client_nonce_ = nonce();
  transport::ClientHandshake handshake_;
  transport::HandshakePacer pacer_;
  transport::JoinIdentity identity_;
  std::function<LicenceProof(std::uint64_t)> licence_provider_;
  std::function<std::string(std::uint64_t)> password_proof_;
  std::future<LicenceProof> licence_pending_;
  std::uint64_t licence_pending_nonce_ = 0;
  std::uint64_t licence_completed_nonce_ = 0;
  bool licence_completed_ = false;
  double licence_refresh_at_ = 0;
  std::string licence_refusal_;
  bool licence_refusal_banned_ = false;
  bool licence_ready() {
    if (!licence_provider_ ||
        handshake_.state() != transport::ClientHandshake::State::AwaitingAccept) {
      return true;
    }
    const auto nonce = handshake_.server_nonce();
    const bool previous_proof =
        licence_completed_ && licence_completed_nonce_ == nonce && !identity_.licence.empty();
    if (licence_completed_ && licence_completed_nonce_ == nonce &&
        clock_seconds() < licence_refresh_at_) {
      return true;
    }
    if (licence_pending_.valid()) {
      if (licence_pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return previous_proof;  // keep the queue heartbeat alive during renewal
      }
      LicenceProof pass;
      try {
        pass = licence_pending_.get();
      } catch (const std::exception&) {
        // An empty proof follows the usual server refusal path, never a bypass.
      }
      if (!pass.refusal.empty()) {
        licence_refusal_ = std::move(pass.refusal);
        licence_refusal_banned_ = pass.banned;
      }
      if (licence_pending_nonce_ == nonce) {
        identity_.licence = std::move(pass.text);
        licence_completed_nonce_ = nonce;
        licence_completed_ = true;
        licence_refresh_at_ = clock_seconds() + std::clamp(pass.refresh_seconds, 1.0, 180.0);
        return true;
      }
    }
    if (!previous_proof) {
      identity_.licence.clear();
    }
    licence_pending_nonce_ = nonce;
    licence_pending_ = std::async(std::launch::async, licence_provider_, nonce);
    return previous_proof;
  }
  // The JOIN_IDENTITY goes just before each CHALLENGE_RESPONSE, echoing its server nonce.
  void send_identity() {
    if (handshake_.state() != transport::ClientHandshake::State::AwaitingAccept) {
      return;
    }
    identity_.echo = handshake_.server_nonce();
    identity_.password_proof = password_proof_ ? password_proof_(identity_.echo) : std::string();
    socket_.send(remote_, transport::encode_join_identity(identity_));
  }
  integration::ClientPeer peer_;
  Datagram pending_;
  std::uint64_t refusals_received_ = 0;
  std::optional<transport::SessionCredentials> session_;
  Datagram disconnect_notice_;
  std::atomic<bool> disconnect_notice_ready_{false};
  std::atomic<bool> disconnected_{false};
  bool offline_ = false;
};
}  // namespace kcd2s::local
