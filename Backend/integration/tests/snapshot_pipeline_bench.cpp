// Opt-in production Server pipeline benchmark. No game or socket processes are started.
// All peers use actual KSTP Connections/ACKs; two sentinel peers additionally apply full mirrors.
// World writes stand in for already-accepted native input. This measures server snapshot work,
// not UDP ingress, 1000 client mirrors, a resource host, native rendering or gameplay acceptance.
#include "../local/animation.hpp"
#include "../local/movement.hpp"
#include "kcd2s/integration/horse.hpp"
#include "kcd2s/integration/npc.hpp"
#include "kcd2s/integration/protocol.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/core/bits.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace kcd2s;

namespace {
struct Options {
  std::string capture;
  unsigned clients = 1000;
  unsigned workers = 16;
  unsigned join_ticks = 2000;
  unsigned steady_ticks = 120;
  unsigned report_ticks = 20;
  unsigned outfit_bytes = 0;
  double clock_hz = 4;
  double refresh_hz = 1;
  bool heterogeneous_acks = true;
  bool profile = false;
};

Options options(int argc, char** argv) {
  Options out;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--all-acks") {
      out.heterogeneous_acks = false;
      continue;
    }
    if (key == "--profile") {
      out.profile = true;
      continue;
    }
    if (i + 1 == argc) {
      throw std::runtime_error("missing option value");
    }
    const std::string value = argv[++i];
    if (key == "--capture") {
      out.capture = value;
      continue;
    }
    std::size_t used = 0;
    const double number = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(number) || number < 0 || number > 1000000) {
      throw std::runtime_error("invalid numeric option");
    }
    if (key == "--clock-hz") {
      out.clock_hz = number;
    } else if (key == "--refresh-hz") {
      out.refresh_hz = number;
    } else {
      if (number != std::floor(number)) {
        throw std::runtime_error("integer option required");
      }
      const auto integer = static_cast<unsigned>(number);
      if (key == "--clients") {
        out.clients = integer;
      } else if (key == "--workers") {
        out.workers = integer;
      } else if (key == "--join-ticks") {
        out.join_ticks = integer;
      } else if (key == "--steady-ticks") {
        out.steady_ticks = integer;
      } else if (key == "--report-ticks") {
        out.report_ticks = integer;
      } else if (key == "--outfit-bytes") {
        out.outfit_bytes = integer;
      } else {
        throw std::runtime_error("unknown option: " + key);
      }
    }
  }
  if (out.capture.empty() || out.clients < 2 || out.clients > 2000 || out.workers < 1 ||
      out.workers > 64 || out.join_ticks < 1 || out.join_ticks > 10000 || out.steady_ticks < 1 ||
      out.steady_ticks > 10000 || out.report_ticks < 1 || out.report_ticks > 1000 ||
      out.outfit_bytes > 8192 || out.clock_hz < 1 || out.clock_hz > 120 || out.refresh_hz > 20) {
    throw std::runtime_error("options outside bounded benchmark limits");
  }
  return out;
}

struct Peer {
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
  std::unique_ptr<transport::Connection> connection;
  std::unique_ptr<replication::Mirror> mirror;
  core::EntityId avatar = 0;
  core::Tick applied = 0;
  bool has_applied = false;
};

struct Window {
  unsigned ticks = 0;
  double receive_ms = 0;
  double build_ms = 0;
  double send_ms = 0;
  double clients_ms = 0;
  double build_max_ms = 0;
  integration::SnapshotBudgetCounters before;
  integration::SnapshotProfileCounters profile_before;
};

double milliseconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

std::string progress_blob(const local::AnimationState& reference, std::uint64_t version) {
  auto state = reference;
  for (auto& layer : state) {
    if (layer.sequence > 0 && layer.clip > 0) {
      layer.params.normalized_time = 0.01f * float(1 + version % 98);
    }
  }
  return local::encode_animation(state);
}

void report(const char* phase, unsigned tick, Window& window, const integration::Server& server,
            const std::vector<Peer>& peers) {
  const auto& after = server.budget_counters();
  const auto snapshots = after.snapshots - window.before.snapshots;
  std::printf(
      "SNAPSHOT_PIPELINE_BENCH {\"phase\":\"%s\",\"tick\":%u,\"window_ticks\":%u,"
      "\"receive_mean_ms\":%.3f,\"build_mean_ms\":%.3f,\"build_max_ms\":%.3f,"
      "\"send_mean_ms\":%.3f,\"ack_clients_mean_ms\":%.3f,\"snapshot_bytes\":%llu,"
      "\"budget_limited_fraction\":%.6f,\"deferred_construct_attempts\":%llu,"
      "\"deferred_update_attempts\":%llu,\"cache_hits\":%llu,\"cache_misses\":%llu,"
      "\"cache_busy\":%llu,\"cache_variants\":%llu,\"cache_memory\":%llu,\"cache_phase\":%llu,"
      "\"sentinel_first_entities\":%zu,\"sentinel_last_entities\":%zu}\n",
      phase, tick, window.ticks, window.receive_ms / window.ticks, window.build_ms / window.ticks,
      window.build_max_ms, window.send_ms / window.ticks, window.clients_ms / window.ticks,
      static_cast<unsigned long long>(after.snapshot_bytes - window.before.snapshot_bytes),
      snapshots ? double(after.budget_limited_snapshots - window.before.budget_limited_snapshots) /
                      double(snapshots)
                : 0.0,
      static_cast<unsigned long long>(after.deferred_constructs -
                                      window.before.deferred_constructs),
      static_cast<unsigned long long>(
          (after.deferred_entries - window.before.deferred_entries) -
          (after.deferred_constructs - window.before.deferred_constructs)),
      static_cast<unsigned long long>(after.update_cache_hits - window.before.update_cache_hits),
      static_cast<unsigned long long>(after.update_cache_misses -
                                      window.before.update_cache_misses),
      static_cast<unsigned long long>(after.update_cache_busy_fallbacks -
                                      window.before.update_cache_busy_fallbacks),
      static_cast<unsigned long long>(after.update_cache_variant_fallbacks -
                                      window.before.update_cache_variant_fallbacks),
      static_cast<unsigned long long>(after.update_cache_memory_fallbacks -
                                      window.before.update_cache_memory_fallbacks),
      static_cast<unsigned long long>(after.update_cache_phase_fallbacks -
                                      window.before.update_cache_phase_fallbacks),
      peers.front().mirror->size(), peers.back().mirror->size());
  const auto& profile = server.snapshot_profile_counters();
  const auto profiled = profile.snapshots - window.profile_before.snapshots;
  if (profiled != 0) {
    const double scale = 1000.0 / window.ticks;
    const auto scalar = [&](double integration::SnapshotProfileCounters::* field) {
      return (profile.*field - window.profile_before.*field) * scale;
    };
    std::array<double, static_cast<std::size_t>(replication::SnapshotPhase::Count)> builder{};
    double measured_builder = 0;
    for (std::size_t i = 0; i < builder.size(); ++i) {
      builder[i] = (profile.builder.seconds[i] - window.profile_before.builder.seconds[i]) * scale;
      measured_builder += builder[i];
    }
    const double snapshot_call =
        scalar(&integration::SnapshotProfileCounters::snapshot_call_seconds);
    std::printf(
        "SNAPSHOT_PIPELINE_PROFILE {\"phase\":\"%s\",\"tick\":%u,\"window_ticks\":%u,"
        "\"profiled_snapshots\":%llu,\"units\":\"elapsed_sum_per_tick_ms_not_CPU\","
        "\"serial_setup\":%.3f,\"interest\":%.3f,\"preparation\":%.3f,\"snapshot_call\":%.3f,"
        "\"scope\":%.3f,\"delta\":%.3f,\"candidates\":%.3f,\"order\":%.3f,"
        "\"selection\":%.3f,\"serialization\":%.3f,\"return_cleanup\":%.3f,\"bookkeeping\":%.3f}\n",
        phase, tick, window.ticks, static_cast<unsigned long long>(profiled),
        scalar(&integration::SnapshotProfileCounters::serial_setup_seconds),
        scalar(&integration::SnapshotProfileCounters::interest_seconds),
        scalar(&integration::SnapshotProfileCounters::preparation_seconds), snapshot_call,
        builder[0], builder[1], builder[2], builder[3], builder[4], builder[5],
        snapshot_call - measured_builder,
        scalar(&integration::SnapshotProfileCounters::bookkeeping_seconds));
  }
  std::fflush(stdout);
  window = {};
  window.before = after;
  window.profile_before = profile;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto opt = options(argc, argv);
    std::ifstream input(opt.capture, std::ios::binary);
    std::array<char, 1221> captured_bytes{};
    input.read(captured_bytes.data(), captured_bytes.size());
    const std::string captured(captured_bytes.data(), std::size_t(input.gcount()));
    const auto decoded = local::decode_animation(captured);
    if (captured.size() > 1220 || !decoded) {
      throw std::runtime_error("capture must be a valid bounded KSA animation payload");
    }
    const auto& animations = *decoded;
    const auto schema = local::player_schema();
    integration::Server server(schema, replication::InterestConfig{64, replication::UpAxis::Z});
    if (opt.workers > 1) {
      server.enable_parallel(opt.workers);
    }
    server.set_interest_hysteresis(25);
    integration::SnapshotBudget budget;
    budget.peer_bytes_per_tick = 262144 / 60;
    budget.total_bytes_per_tick = 50331648 / 60;
    server.set_snapshot_budget(budget);
    server.set_snapshot_profiling(opt.profile);
    auto& world = server.world();
    world.set_tick(1);
    std::vector<Peer> peers(opt.clients);
    for (unsigned i = 0; i < opt.clients; ++i) {
      auto& peer = peers[i];
      peer.links = transport::make_loopback_pair();
      peer.avatar = local::spawn_player(world);
      world.write(peer.avatar, local::Position,
                  core::Vec3{float(i % 32) * 0.6f, float(i / 32) * 0.6f, 14});
      world.write(peer.avatar, local::Animations, captured);
      world.write(peer.avatar, local::BodyVisible, true);
      world.write(peer.avatar, local::PoseValid, true);
      world.write(peer.avatar, local::MotionValid, true);
      world.write(peer.avatar, local::MotionFlags, std::int64_t{1});
      world.write(peer.avatar, local::Posture, std::int64_t{260});
      if (opt.outfit_bytes) {
        world.write(peer.avatar, local::Outfit, std::string(opt.outfit_bytes, 'x'));
      }
      const auto id = server.add_client(peer.links.first.get(), peer.avatar, 150);
      server.set_owner(peer.avatar, id);
      peer.connection = std::make_unique<transport::Connection>(peer.links.second.get());
      peer.connection->set_channel_tier(integration::kChannelReplAck,
                                        transport::DeliveryTier::UnreliableSequenced);
      if (i == 0 || i + 1 == opt.clients) {
        peer.mirror = std::make_unique<replication::Mirror>(schema);
      }
    }
    // Native bodies are authoritative rows only. There is no synthetic AI or gameplay claim.
    std::vector<core::EntityId> bodies;
    for (unsigned i = 0; i < 40; ++i) {
      const bool horse = i >= 32;
      const auto entity = world.spawn(core::type_id(horse ? "NetworkHorse" : "NetworkNpc"));
      world.write(entity, 0, core::Vec3{float(i % 8), float(i / 8), 14});
      bodies.push_back(entity);
    }
    std::printf(
        "SNAPSHOT_PIPELINE_CONFIG {\"clients\":%u,\"rows\":%zu,\"workers\":%u,"
        "\"captured_animation_bytes\":%zu,\"outfit_padding_bytes\":%u,\"peer_share_bytes\":%.3f,"
        "\"clock_hz\":%.3f,\"player_refresh_hz\":%.3f,\"body_refresh_hz\":2,"
        "\"heterogeneous_acks\":%s,\"profile\":%s,\"full_mirror_sentinels\":2,\"native_game\":"
        "false}\n",
        opt.clients, world.entity_count(), opt.workers, captured.size(), opt.outfit_bytes,
        double(budget.total_bytes_per_tick) / opt.clients, opt.clock_hz, opt.refresh_hz,
        opt.heterogeneous_acks ? "true" : "false", opt.profile ? "true" : "false");
    std::vector<std::uint64_t> player_versions(peers.size(), 0);
    std::uint64_t body_version = 0;
    Window window;
    unsigned tick = 0;
    unsigned settled = 0;
    bool joined = false;
    double now = 0;
    const auto step = [&](bool publish) {
      ++tick;
      now += 1.0 / opt.clock_hz;
      world.set_tick(tick + 1);
      if (publish) {
        for (unsigned i = 0; i < peers.size(); ++i) {
          const auto version =
              std::uint64_t(std::floor(now * opt.refresh_hz + double(i % 60) / 60));
          if (version == player_versions[i]) {
            continue;
          }
          player_versions[i] = version;
          world.write(peers[i].avatar, local::Animations, progress_blob(animations, version));
          world.write(peers[i].avatar, local::MotionClock, std::int64_t(now * 1000));
        }
      }
      const auto next_body_version = publish ? std::uint64_t(std::floor(now * 2)) : 0;
      if (next_body_version != body_version || tick == 1) {
        body_version = next_body_version;
        const auto blob = progress_blob(animations, body_version);
        for (unsigned i = 0; i < bodies.size(); ++i) {
          integration::BodyState state;
          state.revision = state.epoch = 1;
          state.source = peers.front().avatar;
          state.sequence = body_version + 1;
          state.position = world.slot(bodies[i])->position;
          state.animations = blob;
          std::vector<std::uint8_t> encoded;
          if (i < 32) {
            integration::NpcState npc;
            static_cast<integration::BodyState&>(npc) = state;
            npc.npc = bodies[i];
            encoded = integration::encode_npc_state(npc);
          } else {
            integration::HorseState horse;
            static_cast<integration::BodyState&>(horse) = state;
            horse.horse = bodies[i];
            encoded = integration::encode_horse(horse);
          }
          if (encoded.empty()) {
            throw std::runtime_error("invalid benchmark body state");
          }
          world.write(bodies[i],
                      i < 32 ? core::FieldId(integration::NpcNativeState)
                             : core::FieldId(integration::HorseNativeState),
                      std::string(encoded.begin(), encoded.end()));
        }
      }
      for (auto& peer : peers) {
        peer.links.first->poll();
        peer.links.second->poll();
      }
      server.tick(now);
      window.receive_ms += server.last_receive_seconds() * 1000;
      window.build_ms += server.last_build_seconds() * 1000;
      window.send_ms += server.last_send_seconds() * 1000;
      window.build_max_ms = std::max(window.build_max_ms, server.last_build_seconds() * 1000);
      const auto client_start = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < peers.size(); ++i) {
        auto& peer = peers[i];
        for (const auto& message : peer.connection->receive(now)) {
          if (message.channel != static_cast<std::uint8_t>(wire::Channel::Snapshot)) {
            continue;
          }
          if (opt.heterogeneous_acks && i % 10 == 0 && tick % 5 == 0) {
            continue;
          }
          core::BitReader header(message.bytes);
          const auto applied = core::Tick(header.read_bits(32));
          header.read_bits(32);
          if (header.overrun() || (peer.mirror && !peer.mirror->apply(message.bytes))) {
            throw std::runtime_error("production snapshot failed sentinel validation");
          }
          peer.applied = applied;
          peer.has_applied = true;
        }
        const unsigned ack_every = opt.heterogeneous_acks ? 1 + i % 4 : 1;
        if (peer.has_applied && tick % ack_every == 0 &&
            (!opt.heterogeneous_acks || (tick + i) % 17 != 0)) {
          peer.connection->queue(integration::kChannelReplAck,
                                 integration::encode_tick(peer.applied));
        }
        peer.connection->flush(now);
      }
      window.clients_ms += milliseconds(client_start);
      ++window.ticks;
    };
    for (unsigned join = 0; join < opt.join_ticks; ++join) {
      const auto before = server.budget_counters().deferred_constructs;
      step(false);
      const bool sentinels_complete = peers.front().mirror->size() == world.entity_count() &&
                                      peers.back().mirror->size() == world.entity_count();
      settled = server.budget_counters().deferred_constructs == before && sentinels_complete
                    ? settled + 1
                    : 0;
      if (window.ticks == opt.report_ticks || settled == 5 || join + 1 == opt.join_ticks) {
        report("construct-drain", tick, window, server, peers);
      }
      if (settled == 5) {
        joined = true;
        break;
      }
    }
    for (unsigned frame = 0; frame < opt.steady_ticks; ++frame) {
      step(true);
      if (window.ticks == opt.report_ticks || frame + 1 == opt.steady_ticks) {
        report("ongoing-idle-refresh", tick, window, server, peers);
      }
    }
    std::printf(
        "SNAPSHOT_PIPELINE_RESULT {\"construct_backlog_drained\":%s,\"sentinel_decode_ok\":true,"
        "\"ticks\":%u,\"simulated_seconds\":%.3f,\"native_acceptance\":false}\n",
        joined ? "true" : "false", tick, now);
    return joined ? 0 : 3;
  } catch (const std::exception& error) {
    std::fprintf(stderr,
                 "%s\nusage: bench_integration_snapshot_pipeline --capture file.ksa "
                 "[--clients 1000 --workers 16 --clock-hz 4 --refresh-hz 1 "
                 "--join-ticks 2000 --steady-ticks 120 --report-ticks 20 --outfit-bytes 0 "
                 "--all-acks --profile]\n",
                 error.what());
    return 2;
  }
}
