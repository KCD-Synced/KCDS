// What the server sends, as one digest: a fixed, seeded session of many clients against the
// production Server, every datagram the server sends to each client hashed in order.
//
// It guards changes that must not change what the game receives (faster data structures, fewer
// copies, other ways to build the same snapshots): run it on the build before and after the change
// with the same options, and the digests must be the same. Clients are real ClientPeers on
// simulated links (seeded loss), the clock steps by fixed ticks, and everything the clients do is
// derived from their index and the tick: they join over the first seconds, walk around their
// village, change their animations, chat, and some leave and come back. Server-authored NPCs and
// horses move through the same world.
//
// The digest covers the Server class (receive, owner input, snapshots, reliable messages,
// retransmits). The resource host is a separate process and is not part of it.
#ifndef KCDS_INTEGRATION_TESTS_SERVER_OUTPUT_DIGEST_HPP
#define KCDS_INTEGRATION_TESTS_SERVER_OUTPUT_DIGEST_HPP

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../local/animation.hpp"
#include "../local/movement.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/horse.hpp"
#include "kcd2s/integration/npc.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/replication/interest.hpp"
#include "kcd2s/transport/datagram.hpp"

namespace kcds::digest {

namespace kcd2s = ::kcd2s;

struct DigestOptions {
  unsigned clients = 64;
  unsigned seconds = 20;
  // Snapshot worker threads (Server::enable_parallel); 1 builds serially.
  unsigned workers = 1;
  // Loss on both directions of every link, from 0 to 1 (seeded, so the same every run).
  double loss = 0.02;
  unsigned npcs = 16;
  unsigned horses = 8;
};

struct Digest {
  std::uint64_t value = 0;
  std::uint64_t datagrams = 0;
  std::uint64_t bytes = 0;
  // The entities the connected clients' mirrors hold at the end: a run in which nothing
  // replicated would digest as well, so the test checks this too.
  std::uint64_t mirrored_entities = 0;
  // Wall time spent in Server::tick alone (not the simulated clients), for comparing speed.
  double server_seconds = 0.0;
};

// FNV-1a, 64 bits.
inline void mix(std::uint64_t& hash, const std::uint8_t* data, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
}

inline void mix_number(std::uint64_t& hash, std::uint64_t number) {
  std::uint8_t bytes[8];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = std::uint8_t(number >> (8 * i));
  }
  mix(hash, bytes, sizeof(bytes));
}

// The server's side of one client's link: every datagram it sends is hashed, with the tick it
// was sent on, before it goes on to the simulated link.
class RecordingLink : public kcd2s::transport::IDatagramLink {
 public:
  RecordingLink(std::unique_ptr<kcd2s::transport::SimLink> inner, const std::uint64_t& tick)
      : inner_(std::move(inner)), tick_(tick) {
  }

  using IDatagramLink::send;
  void send(const std::uint8_t* data, std::size_t len) override {
    mix_number(hash_, tick_);
    mix_number(hash_, len);
    mix(hash_, data, len);
    datagrams_ += 1;
    bytes_ += len;
    inner_->send(data, len);
  }
  bool recv(kcd2s::transport::Datagram& out) override {
    return inner_->recv(out);
  }
  void poll() override {
    inner_->poll();
  }

  std::uint64_t hash() const {
    return hash_;
  }
  std::uint64_t datagrams() const {
    return datagrams_;
  }
  std::uint64_t bytes() const {
    return bytes_;
  }

 private:
  std::unique_ptr<kcd2s::transport::SimLink> inner_;
  const std::uint64_t& tick_;
  std::uint64_t hash_ = 14695981039346656037ull;
  std::uint64_t datagrams_ = 0;
  std::uint64_t bytes_ = 0;
};

namespace detail {

constexpr double kTickRate = 60.0;
// Villages far enough apart that each is out of the others' 150 m view.
constexpr unsigned kVillages = 8;
constexpr float kVillageSpacing = 400.0f;
constexpr float kViewRange = 150.0f;
// Every this many seconds a few clients leave, and come back this many seconds later.
constexpr unsigned kChurnEverySeconds = 10;
constexpr unsigned kAwaySeconds = 3;

struct Session {
  std::unique_ptr<RecordingLink> server_side;
  std::unique_ptr<kcd2s::transport::SimLink> client_side;
  std::unique_ptr<kcd2s::integration::ClientPeer> peer;
  kcd2s::integration::PeerId id = 0;
  kcd2s::core::EntityId avatar = 0;
  bool ready = false;
};

struct Client {
  std::unique_ptr<Session> session;
  // The digests of the sessions that ended, in order.
  std::uint64_t ended_hash = 14695981039346656037ull;
  std::uint64_t ended_datagrams = 0;
  std::uint64_t ended_bytes = 0;
  std::uint32_t sessions = 0;
};

inline kcd2s::core::Vec3 village_of(unsigned index) {
  const unsigned village = index % kVillages;
  return kcd2s::core::Vec3{float(village % 3) * kVillageSpacing,
                           float(village / 3) * kVillageSpacing, 14.0f};
}

// Where client `index` walks at `time`: a circle of its own around its village's centre.
inline kcd2s::core::Vec3 walk(unsigned index, double time) {
  const auto centre = village_of(index);
  const float radius = 5.0f + float(index % 7) * 3.0f;
  const double phase = time * (0.2 + 0.01 * double(index % 5)) + double(index);
  return kcd2s::core::Vec3{centre.x + radius * float(std::cos(phase)),
                           centre.y + radius * float(std::sin(phase)), centre.z};
}

inline std::string animation_of(unsigned index, std::uint64_t version) {
  kcd2s::local::AnimationState state{};
  state[0].sequence = 1 + version;
  state[0].clip = 101 + int(index % 9);
  state[0].params.flags = 0x102;
  state[0].params.normalized_time = 0.01f * float(1 + version % 98);
  return kcd2s::local::encode_animation(state);
}

inline std::uint64_t link_seed(unsigned index, std::uint32_t session, unsigned direction) {
  return 0x9E3779B97F4A7C15ull ^ (std::uint64_t(index) << 20) ^ (std::uint64_t(session) << 8) ^
         direction;
}

}  // namespace detail

inline Digest run_digest(const DigestOptions& options) {
  using namespace detail;
  namespace integration = kcd2s::integration;
  namespace local = kcd2s::local;

  const auto schema = local::player_schema();
  integration::Server server(schema,
                             kcd2s::replication::InterestConfig{64, kcd2s::replication::UpAxis::Z});
  if (options.workers > 1) {
    server.enable_parallel(options.workers);
  }
  server.set_interest_hysteresis(25);
  integration::SnapshotBudget budget;
  budget.peer_bytes_per_tick = 262144 / 60;
  budget.total_bytes_per_tick = 50331648 / 60;
  server.set_snapshot_budget(budget);
  // Every peer gets its snapshot every tick, however long the builds take on this machine: the
  // stride that follows the build time would make a loaded run depend on the machine's speed.
  server.hold_snapshot_stride(1);
  auto& world = server.world();
  world.set_tick(1);

  std::uint64_t tick = 0;
  double server_seconds = 0.0;
  double now = 0.0;

  // Server-authored bodies: NPCs and horses standing in the villages, moved twice a second.
  std::vector<kcd2s::core::EntityId> bodies;
  for (unsigned i = 0; i < options.npcs + options.horses; ++i) {
    const bool horse = i >= options.npcs;
    const auto entity = world.spawn(kcd2s::core::type_id(horse ? "NetworkHorse" : "NetworkNpc"));
    world.write(entity, 0, village_of(i));
    bodies.push_back(entity);
  }

  std::vector<Client> clients(options.clients);
  const auto join = [&](unsigned index) {
    auto& client = clients[index];
    auto session = std::make_unique<Session>();
    kcd2s::transport::LinkParams to_client;
    to_client.loss = options.loss;
    to_client.seed = link_seed(index, client.sessions, 1);
    kcd2s::transport::LinkParams to_server;
    to_server.loss = options.loss;
    to_server.seed = link_seed(index, client.sessions, 2);
    auto links = kcd2s::transport::make_sim_pair(to_client, to_server);
    session->server_side = std::make_unique<RecordingLink>(std::move(links.first), tick);
    session->client_side = std::move(links.second);
    session->avatar = local::spawn_player(world);
    world.write(session->avatar, local::Position, walk(index, now));
    session->id = server.add_client(session->server_side.get(), session->avatar, kViewRange);
    server.set_owner(session->avatar, session->id);
    server.configure_world(session->id, {});
    session->peer = std::make_unique<integration::ClientPeer>(schema, session->client_side.get());
    client.sessions += 1;
    client.session = std::move(session);
  };
  const auto leave = [&](unsigned index) {
    auto& client = clients[index];
    if (!client.session) {
      return;
    }
    const auto& link = *client.session->server_side;
    mix_number(client.ended_hash, link.hash());
    client.ended_datagrams += link.datagrams();
    client.ended_bytes += link.bytes();
    server.remove_client(client.session->id);
    world.despawn(client.session->avatar);
    client.session.reset();
  };

  const std::uint64_t ticks = std::uint64_t(options.seconds * kTickRate);
  for (tick = 1; tick <= ticks; ++tick) {
    now = double(tick) / kTickRate;
    world.set_tick(world.tick() + 1);
    const std::uint64_t second = tick / std::uint64_t(kTickRate);
    const bool second_starts = tick % std::uint64_t(kTickRate) == 0;

    // Joins: a tenth of the clients each 0.2 s at the start; churn: every kChurnEverySeconds the
    // clients whose index matches leave, and come back kAwaySeconds later.
    for (unsigned i = 0; i < clients.size(); ++i) {
      const std::uint64_t join_tick = 1 + std::uint64_t(i % 10) * 12;
      if (clients[i].sessions == 0 && tick == join_tick) {
        join(i);
      }
    }
    if (second_starts && second > 0 && second % kChurnEverySeconds == 0) {
      for (unsigned i = unsigned(second / kChurnEverySeconds) % 13; i < clients.size(); i += 13) {
        leave(i);
      }
    }
    if (second_starts && second > kAwaySeconds &&
        (second - kAwaySeconds) % kChurnEverySeconds == 0) {
      for (unsigned i = 0; i < clients.size(); ++i) {
        if (!clients[i].session && clients[i].sessions > 0) {
          join(i);
        }
      }
    }

    // What the players send: their walk every tick, a new animation every half second, a chat
    // line now and then.
    for (unsigned i = 0; i < clients.size(); ++i) {
      auto* session = clients[i].session.get();
      if (!session) {
        continue;
      }
      auto& peer = *session->peer;
      if (!session->ready && peer.world_config()) {
        session->ready = peer.mark_world_ready(peer.world_config()->revision);
      }
      if (!session->ready || !peer.owns(session->avatar)) {
        continue;
      }
      const auto here = walk(i, now);
      const auto ahead = walk(i, now + 1.0 / kTickRate);
      std::vector<std::pair<kcd2s::core::FieldId, kcd2s::replication::FieldValue>> input;
      input.emplace_back(local::Position, here);
      input.emplace_back(local::Velocity,
                         kcd2s::core::Vec3{(ahead.x - here.x) * float(kTickRate),
                                           (ahead.y - here.y) * float(kTickRate), 0.0f});
      input.emplace_back(local::MotionClock, std::int64_t(now * 1000.0));
      input.emplace_back(local::PoseValid, true);
      input.emplace_back(local::MotionValid, true);
      input.emplace_back(local::BodyVisible, true);
      if (tick % 30 == i % 30) {
        input.emplace_back(local::Animations, animation_of(i, tick / 30));
      }
      peer.send_input(session->avatar, input);
      if (second_starts && (second + i) % 7 == 0) {
        peer.send_chat(session->avatar,
                       "line " + std::to_string(i) + " at " + std::to_string(second));
      }
    }

    // The NPCs and horses move twice a second.
    if (tick % 30 == 0) {
      for (unsigned i = 0; i < bodies.size(); ++i) {
        const auto centre = village_of(i);
        const double phase = now * 0.3 + double(i);
        world.write(bodies[i], 0,
                    kcd2s::core::Vec3{centre.x + 12.0f * float(std::cos(phase)),
                                      centre.y + 12.0f * float(std::sin(phase)), centre.z});
      }
    }

    const auto tick_started = std::chrono::steady_clock::now();
    server.tick(now);
    server_seconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tick_started).count();
    for (auto& client : clients) {
      if (client.session) {
        client.session->peer->tick(now);
      }
    }
  }

  Digest digest;
  std::uint64_t value = 14695981039346656037ull;
  for (unsigned i = 0; i < clients.size(); ++i) {
    auto& client = clients[i];
    std::uint64_t hash = client.ended_hash;
    std::uint64_t datagrams = client.ended_datagrams;
    std::uint64_t bytes = client.ended_bytes;
    if (client.session) {
      mix_number(hash, client.session->server_side->hash());
      datagrams += client.session->server_side->datagrams();
      bytes += client.session->server_side->bytes();
      digest.mirrored_entities += client.session->peer->mirror().size();
    }
    mix_number(value, hash);
    digest.datagrams += datagrams;
    digest.bytes += bytes;
  }
  digest.value = value;
  digest.server_seconds = server_seconds;
  return digest;
}

}  // namespace kcds::digest

#endif  // KCDS_INTEGRATION_TESTS_SERVER_OUTPUT_DIGEST_HPP
