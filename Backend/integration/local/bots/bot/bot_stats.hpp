#pragma once
// What one bot measures during a run, merged per thread and for the whole run at the end.
#include <array>
#include <cstdint>
#include <map>
#include <string>

#include "../../metrics/duration_histogram.hpp"

namespace kcd2s::local::bots {

// Distance bands of the server's distance rates (KCDS_SERVER_DISTANCE_RATES): full rate to 25 m,
// 20 Hz to 50 m, 5 Hz to 100 m, 2 Hz beyond. "near" is 0..25 m, "mid" 25..100 m, "far" 100 m on.
enum class Band : std::size_t { Near, Mid, Far };
inline constexpr std::array<const char*, 3> kBandNames{"near", "mid", "far"};

inline Band band_of(double metres) {
  if (metres < 25.0) {
    return Band::Near;
  }
  if (metres < 100.0) {
    return Band::Mid;
  }
  return Band::Far;
}

struct BotStats {
  // Joins: attempts, how many reached a spawned avatar, refused and timed out ones, and the time
  // from the first datagram to the avatar.
  std::uint64_t join_attempts = 0;
  std::uint64_t spawned = 0;
  std::uint64_t refused_joins = 0;
  std::uint64_t timed_out_joins = 0;
  std::map<std::string, std::uint64_t> refusal_reasons;
  metrics::DurationHistogram connect_to_spawn_ms;

  // Replication: snapshots applied, input-to-other-bot latency (snapshot age), and the time
  // between updates of another player by distance band.
  std::uint64_t snapshots = 0;
  metrics::DurationHistogram snapshot_age_ms;
  std::array<metrics::DurationHistogram, 3> update_interval_ms;

  // Seconds it spent spawned and playing (its rates are per this time).
  double playing_seconds = 0.0;

  // The server's corrections (its movement check putting the bot back) and teleports.
  std::uint64_t corrections = 0;
  std::uint64_t teleports = 0;

  // What the bot did.
  std::uint64_t inputs = 0;
  std::uint64_t chat_lines = 0;
  std::uint64_t emotes = 0;
  std::uint64_t swings = 0;
  std::uint64_t contacts_sent = 0;
  std::uint64_t contacts_received = 0;
  std::uint64_t blocks = 0;
  std::uint64_t effects_sent = 0;
  std::uint64_t shots = 0;
  std::uint64_t voice_frames = 0;
  std::uint64_t horses = 0;
  std::uint64_t mounts = 0;
  std::uint64_t leaves = 0;

  void merge(const BotStats& other) {
    join_attempts += other.join_attempts;
    spawned += other.spawned;
    refused_joins += other.refused_joins;
    timed_out_joins += other.timed_out_joins;
    for (const auto& [reason, count] : other.refusal_reasons) {
      refusal_reasons[reason] += count;
    }
    connect_to_spawn_ms.merge(other.connect_to_spawn_ms);
    snapshots += other.snapshots;
    snapshot_age_ms.merge(other.snapshot_age_ms);
    for (std::size_t band = 0; band < update_interval_ms.size(); ++band) {
      update_interval_ms[band].merge(other.update_interval_ms[band]);
    }
    playing_seconds += other.playing_seconds;
    corrections += other.corrections;
    teleports += other.teleports;
    inputs += other.inputs;
    chat_lines += other.chat_lines;
    emotes += other.emotes;
    swings += other.swings;
    contacts_sent += other.contacts_sent;
    contacts_received += other.contacts_received;
    blocks += other.blocks;
    effects_sent += other.effects_sent;
    shots += other.shots;
    voice_frames += other.voice_frames;
    horses += other.horses;
    mounts += other.mounts;
    leaves += other.leaves;
  }
};

}  // namespace kcd2s::local::bots
