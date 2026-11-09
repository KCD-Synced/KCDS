#pragma once
// Scaling settings of the local KSTP host (phase 2). Every value can be set through a
// KCDS_SERVER_* environment variable; an unset variable keeps the default and an invalid one is
// reported and ignored. Portable so the parser is covered by the Linux unit tests.
//
//   KCDS_SERVER_MAX_PEERS                 connection cap, 1-1024 (default 32); handshakes above it
//                                         get CONNECT_REFUSED (server full)
//   KCDS_SERVER_INTEREST_RADIUS           metres in the horizontal plane, 1-100000 (default 150)
//   KCDS_SERVER_INTEREST_HYSTERESIS       extra leave distance in metres, 0-1000 (default 25)
//   KCDS_SERVER_MAX_CHARACTERS            per-client players + NPCs + horses, including the local
//                                         player, 0-4096; 0 leaves admission disabled (default)
//   KCDS_SERVER_PEER_SNAPSHOT_BYTES_PER_S snapshot payload budget per peer (default 262144)
//   KCDS_SERVER_SNAPSHOT_BYTES_PER_S      aggregate snapshot payload budget (default 50331648)
//   KCDS_SERVER_MAX_DATAGRAM              largest UDP payload sent, 576-65507 (default 1200);
//                                         larger KSTP datagrams are fragmented
//   KCDS_SERVER_VOICE_MODE                embedded (default) or external; external also takes
//                                         KCDS_SERVER_VOICE_URL, _ID, _SECRET and _PUBLIC
//   KCDS_SERVER_SNAPSHOT_WORKERS          parallel snapshot build threads, 0-64; 0 or 1 is a
//                                         serial build. Unset: the machine's hardware threads
//                                         but one, at least 2 and at most 16
//                                         (default_snapshot_workers), so a dedicated server uses
//                                         its cores without being told to
//   KCDS_SERVER_MOVEMENT_CHECK            off, observe (default: impossible player steps are logged
//                                         with their reason) or enforce (and the player is put
//                                         back where its last possible step ended)
//   KCDS_SERVER_DISTANCE_RATES            on (default) or off: far entities' movement goes to a
//                                         client less often (full rate to 25 m, 20 Hz to 50 m,
//                                         5 Hz to 100 m, 2 Hz beyond)
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "kcd2s/integration/server.hpp"
#include "kcd2s/transport/fragment.hpp"

namespace kcd2s::local::config {

inline constexpr unsigned kDefaultMaxPeers = 32;
inline constexpr unsigned kMaxPeersLimit = 1024;
inline constexpr float kDefaultInterestRadius = 150.0f;
inline constexpr float kDefaultInterestHysteresis = 25.0f;
inline constexpr std::uint64_t kDefaultPeerSnapshotBytesPerSecond = 256u * 1024u;
inline constexpr std::uint64_t kDefaultTotalSnapshotBytesPerSecond = 48ull * 1024u * 1024u;
inline constexpr std::size_t kDefaultMaxDatagramBytes = transport::kDefaultMtu;
inline constexpr unsigned kMaxSnapshotWorkers = 64;
// The most snapshot workers the default picks; more can be set by hand.
inline constexpr unsigned kDefaultSnapshotWorkerLimit = 16;

// The snapshot workers a host gets when KCDS_SERVER_SNAPSHOT_WORKERS is unset: every hardware
// thread but one (the network tick and the resource host keep a core), at least 2, at most
// kDefaultSnapshotWorkerLimit; a single-thread machine builds serially.
inline unsigned default_snapshot_workers(unsigned hardware_threads) {
  if (hardware_threads < 2) {
    return 0;
  }
  return std::clamp(hardware_threads - 1, 2u, kDefaultSnapshotWorkerLimit);
}

struct HostSettings {
  double tick_rate_hz = 60.0;
  unsigned max_peers = kDefaultMaxPeers;
  float interest_radius = kDefaultInterestRadius;
  float interest_hysteresis = kDefaultInterestHysteresis;
  unsigned max_characters = 0;
  std::uint64_t peer_snapshot_bytes_per_second = kDefaultPeerSnapshotBytesPerSecond;
  std::uint64_t total_snapshot_bytes_per_second = kDefaultTotalSnapshotBytesPerSecond;
  std::size_t max_datagram_bytes = kDefaultMaxDatagramBytes;
  unsigned snapshot_workers = 0;
  bool distance_rates = true;
  integration::MovementCheckMode movement_check = integration::MovementCheckMode::Observe;
  integration::MeleeReachMode melee_reach_check = integration::MeleeReachMode::Observe;
  // Voice chat: built in, or on a standalone kcds-voice (KCDS-DOCS/voice.md).
  bool voice_external = false;
  std::string voice_url;
  std::string voice_server_id;
  std::string voice_secret;
  std::string voice_public_address;

  // Per-tick snapshot budget derived from the per-second rates and the tick rate.
  integration::SnapshotBudget snapshot_budget() const {
    integration::SnapshotBudget budget;
    budget.peer_bytes_per_tick =
        static_cast<std::uint32_t>(double(peer_snapshot_bytes_per_second) / tick_rate_hz);
    budget.total_bytes_per_tick =
        static_cast<std::uint64_t>(double(total_snapshot_bytes_per_second) / tick_rate_hz);
    return budget;
  }

  // Interest grid cell edge: about half the interest radius, so a query scans a small box.
  float interest_cell_size() const {
    return std::clamp(interest_radius / 2.0f, 16.0f, 512.0f);
  }

  // Datagrams drained from the socket per tick: enough for every peer's input, its fragments and
  // acknowledgements, never below the original 1024.
  unsigned receive_budget() const {
    return std::max(1024u, 8u * max_peers);
  }

  bool parallel_snapshots() const {
    return snapshot_workers >= 2;
  }
};

namespace detail {

inline std::optional<double> parse_bounded(const char* text, double min, double max) {
  if (!text || !*text) {
    return std::nullopt;
  }
  char* end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || *end != '\0' || !(value >= min) || !(value <= max)) {
    return std::nullopt;
  }
  return value;
}

inline std::optional<std::uint64_t> parse_whole(const char* text, double min, double max) {
  const auto value = parse_bounded(text, min, max);
  if (!value || *value != static_cast<double>(static_cast<std::uint64_t>(*value))) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(*value);
}

}  // namespace detail

// Returns the value of an environment variable, or null when it is not set.
using SettingLookup = std::function<std::optional<std::string>(const char* name)>;

// The voice settings. External without all it needs stays built in, and says so.
inline void read_voice_settings(const SettingLookup& lookup, HostSettings& settings,
                                std::vector<std::string>& problems) {
  const std::string mode = lookup("KCDS_SERVER_VOICE_MODE").value_or("embedded");
  if (mode != "embedded" && mode != "external") {
    problems.push_back("Ignoring invalid KCDS_SERVER_VOICE_MODE=" + mode);
    return;
  }
  if (mode == "embedded") {
    return;
  }
  settings.voice_url = lookup("KCDS_SERVER_VOICE_URL").value_or("");
  settings.voice_server_id = lookup("KCDS_SERVER_VOICE_ID").value_or("");
  settings.voice_secret = lookup("KCDS_SERVER_VOICE_SECRET").value_or("");
  settings.voice_public_address = lookup("KCDS_SERVER_VOICE_PUBLIC").value_or("");
  if (settings.voice_url.empty() || settings.voice_server_id.empty() ||
      settings.voice_secret.empty()) {
    problems.push_back(
        "Voice stays built in: voiceMode external needs voiceUrl, voiceServerId and voiceSecret");
    return;
  }
  settings.voice_external = true;
}

// Reads every KCDS_SERVER_* scaling setting through `lookup`. Invalid values keep the default and
// add a message to `problems`. `hardware_threads`: the machine's, for the snapshot workers'
// default.
inline HostSettings read_host_settings(
    double tick_rate_hz, const SettingLookup& lookup, std::vector<std::string>& problems,
    unsigned hardware_threads = std::thread::hardware_concurrency()) {
  HostSettings settings;
  settings.tick_rate_hz = tick_rate_hz;
  settings.snapshot_workers = default_snapshot_workers(hardware_threads);

  auto whole = [&](const char* name, double min, double max, auto& target) {
    const auto text = lookup(name);
    if (!text) {
      return;
    }
    const auto value = detail::parse_whole(text->c_str(), min, max);
    if (!value) {
      problems.push_back(std::string("Ignoring invalid ") + name + "=" + *text);
      return;
    }
    target = static_cast<std::remove_reference_t<decltype(target)>>(*value);
  };
  auto number = [&](const char* name, double min, double max, float& target) {
    const auto text = lookup(name);
    if (!text) {
      return;
    }
    const auto value = detail::parse_bounded(text->c_str(), min, max);
    if (!value) {
      problems.push_back(std::string("Ignoring invalid ") + name + "=" + *text);
      return;
    }
    target = static_cast<float>(*value);
  };

  whole("KCDS_SERVER_MAX_PEERS", 1, kMaxPeersLimit, settings.max_peers);
  number("KCDS_SERVER_INTEREST_RADIUS", 1.0, 100000.0, settings.interest_radius);
  number("KCDS_SERVER_INTEREST_HYSTERESIS", 0.0, 1000.0, settings.interest_hysteresis);
  whole("KCDS_SERVER_MAX_CHARACTERS", 0, 4096, settings.max_characters);
  whole("KCDS_SERVER_PEER_SNAPSHOT_BYTES_PER_S", 1024, 64.0 * 1024 * 1024,
        settings.peer_snapshot_bytes_per_second);
  whole("KCDS_SERVER_SNAPSHOT_BYTES_PER_S", 1024, 1024.0 * 1024 * 1024,
        settings.total_snapshot_bytes_per_second);
  whole("KCDS_SERVER_MAX_DATAGRAM", 576, 65507, settings.max_datagram_bytes);
  whole("KCDS_SERVER_SNAPSHOT_WORKERS", 0, kMaxSnapshotWorkers, settings.snapshot_workers);
  if (const auto rates = lookup("KCDS_SERVER_DISTANCE_RATES")) {
    if (*rates == "on" || *rates == "1" || *rates == "true") {
      settings.distance_rates = true;
    } else if (*rates == "off" || *rates == "0" || *rates == "false") {
      settings.distance_rates = false;
    } else {
      problems.push_back("Ignoring invalid KCDS_SERVER_DISTANCE_RATES=" + *rates);
    }
  }
  if (const auto mode = lookup("KCDS_SERVER_MOVEMENT_CHECK")) {
    if (*mode == "off") {
      settings.movement_check = integration::MovementCheckMode::Off;
    } else if (*mode == "observe") {
      settings.movement_check = integration::MovementCheckMode::Observe;
    } else if (*mode == "enforce") {
      settings.movement_check = integration::MovementCheckMode::Enforce;
    } else {
      problems.push_back("Ignoring invalid KCDS_SERVER_MOVEMENT_CHECK=" + *mode);
    }
  }
  if (const auto mode = lookup("KCDS_SERVER_MELEE_REACH_CHECK")) {
    if (*mode == "off") {
      settings.melee_reach_check = integration::MeleeReachMode::Off;
    } else if (*mode == "observe") {
      settings.melee_reach_check = integration::MeleeReachMode::Observe;
    } else if (*mode == "enforce") {
      settings.melee_reach_check = integration::MeleeReachMode::Enforce;
    } else {
      problems.push_back("Ignoring invalid KCDS_SERVER_MELEE_REACH_CHECK=" + *mode);
    }
  }
  read_voice_settings(lookup, settings, problems);
  return settings;
}

}  // namespace kcd2s::local::config
