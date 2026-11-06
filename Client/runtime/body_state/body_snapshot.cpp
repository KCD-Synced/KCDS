// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "body_snapshot.hpp"

#include "../characters/animation_samples.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../knockout/body_state.hpp"
#include "../npcs/native_npcs.hpp"
#include "../replicas/character_retirement.hpp"
#include "../replicas/remote.hpp"
#include "../riding/native_horses.hpp"
#include "../stats/player_command_binding.hpp"
#include "../testing/test_hooks.hpp"
#include "body_readings.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace kcds::game_client {
namespace {

namespace characters = kcds::runtime::characters;

struct CharacterSampleCounts {
  std::size_t known = 0;
  std::size_t sampled = 0;
  std::size_t identities = 0;
  std::size_t characters = 0;
  std::size_t skeletons = 0;
  std::size_t fifo_bodies = 0;
  std::size_t fifo_layers = 0;
  std::size_t fifo_clips = 0;
  std::size_t rejected_layers = 0;
  std::size_t comparable = 0;
  std::size_t progressing = 0;
  std::size_t body_comparable = 0;
  std::size_t body_progressing = 0;
  std::size_t normalized = 0;
  float progress_min = 1;
  float progress_max = 0;
};

void sample_character(unsigned engine_id, void* actor, void* entity_system,
                      CharacterSampleCounts& counts, characters::AnimationSamples& samples) {
  ++counts.sampled;
  void* entity = entity_system ? get_entity(entity_system, engine_id) : nullptr;
  if (!actor || !entity ||
      *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 56) != entity) {
    return;
  }
  ++counts.identities;
  // Same slot-0 character and SkeletonAnim contracts as replica_tick/native_npcs/native_horses.
  void* character = slot<void*(__fastcall*)(void*, unsigned)>(entity, 101)(entity, 0);
  if (!character) {
    return;
  }
  ++counts.characters;
  void* skeleton = slot<void*(__fastcall*)(void*)>(character, 5)(character);
  if (!skeleton) {
    return;
  }
  ++counts.skeletons;
  characters::AnimationBodySample current;
  current.engine_id = engine_id;
  current.entity = reinterpret_cast<std::uintptr_t>(entity);
  current.character = reinterpret_cast<std::uintptr_t>(character);
  current.skeleton = reinterpret_cast<std::uintptr_t>(skeleton);
  const bool progress_available =
      native_animation_progress && (*reinterpret_cast<void***>(skeleton))[28] ==
                                       reinterpret_cast<void*>(native_animation_progress);
  bool has_fifo = false;
  for (unsigned layer = 0; layer < current.layers.size(); ++layer) {
    // Existing capture_existing_animations contracts: FIFO count v15, entry v18, clip at +16,
    // normalized whole-clip progress v28. Do not call capture itself: it mutates owner state.
    const auto count = slot<unsigned(__fastcall*)(void*, unsigned)>(skeleton, 15)(skeleton, layer);
    if (count > 16) {
      ++counts.rejected_layers;
      continue;
    }
    if (count == 0) {
      continue;
    }
    has_fifo = true;
    ++counts.fifo_layers;
    counts.fifo_clips += count;
    const auto* raw = slot<const std::uint8_t*(__fastcall*)(void*, unsigned, unsigned)>(
        skeleton, 18)(skeleton, layer, 0);
    if (!raw || !progress_available) {
      continue;
    }
    auto& observed = current.layers[layer];
    observed.entry = reinterpret_cast<std::uintptr_t>(raw);
    std::memcpy(&observed.clip, raw + 16, sizeof(observed.clip));
    // Observe the head, not a newly queued tail. A FIFO or skeleton alone is not rendered-pose
    // evidence; only subsequent samples of the same clip/entry can show native clock movement.
    observed.progress = native_animation_progress(skeleton, raw);
    if (observed.valid()) {
      ++counts.normalized;
      counts.progress_min = std::min(counts.progress_min, observed.progress);
      counts.progress_max = std::max(counts.progress_max, observed.progress);
    }
  }
  counts.fifo_bodies += has_fifo;
  if (const auto comparison = samples.observe(current)) {
    counts.comparable += comparison->comparable_layers != 0;
    counts.progressing += comparison->progressing_layers != 0;
    counts.body_comparable += comparison->body_comparable;
    counts.body_progressing += comparison->body_progressing;
  }
}

void log_character_snapshot(std::uint64_t sample, void* entity_system) {
  static characters::AnimationSamples samples;
  samples.begin(entity_system);
  CharacterSampleCounts players, npcs, horses;
  std::size_t inspected = 0;
  const auto collect = [&](const auto& bodies, CharacterSampleCounts& counts) {
    counts.known = bodies.size();
    for (const auto& [id, body] : bodies) {
      if (inspected == characters::kMaximumAnimationSamples) {
        break;
      }
      ++inspected;
      sample_character(body.engine_id, body.actor, entity_system, counts, samples);
    }
  };
  collect(remotes, players);
  collect(native_npcs, npcs);
  collect(native_horses, horses);
  samples.finish();
  const auto allocations = character_allocation_counts();
  const auto known = players.known + npcs.known + horses.known;
  const auto normalized = players.normalized + npcs.normalized + horses.normalized;
  const auto minimum = std::min({players.progress_min, npcs.progress_min, horses.progress_min});
  const auto maximum = std::max({players.progress_max, npcs.progress_max, horses.progress_max});
  log("CHARACTER SNAPSHOT sample=%llu types=player/npc/horse known=%zu/%zu/%zu "
      "sampled=%zu/%zu/%zu identities=%zu/%zu/%zu characters=%zu/%zu/%zu "
      "skeletons=%zu/%zu/%zu fifo_bodies=%zu/%zu/%zu fifo_layers=%zu/%zu/%zu "
      "fifo_clips=%zu/%zu/%zu comparable=%zu/%zu/%zu progressing=%zu/%zu/%zu "
      "body_comparable=%zu/%zu/%zu body_progressing=%zu/%zu/%zu "
      "normalized_heads=%zu progress_min=%.6f progress_max=%.6f rejected_layers=%zu "
      "admitted=%zu active=%zu retiring=%zu truncated=%zu bound=%zu tick=%llu",
      sample, players.known, npcs.known, horses.known, players.sampled, npcs.sampled,
      horses.sampled, players.identities, npcs.identities, horses.identities, players.characters,
      npcs.characters, horses.characters, players.skeletons, npcs.skeletons, horses.skeletons,
      players.fifo_bodies, npcs.fifo_bodies, horses.fifo_bodies, players.fifo_layers,
      npcs.fifo_layers, horses.fifo_layers, players.fifo_clips, npcs.fifo_clips, horses.fifo_clips,
      players.comparable, npcs.comparable, horses.comparable, players.progressing, npcs.progressing,
      horses.progressing, players.body_comparable, npcs.body_comparable, horses.body_comparable,
      players.body_progressing, npcs.body_progressing, horses.body_progressing, normalized,
      normalized ? minimum : -1.0f, normalized ? maximum : -1.0f,
      players.rejected_layers + npcs.rejected_layers + horses.rejected_layers, allocations.admitted,
      allocations.active, allocations.retiring, known - inspected,
      characters::kMaximumAnimationSamples, GetTickCount64());
}

}  // namespace

void log_native_body_sample(std::uint64_t sample, core::EntityId player, unsigned engine_id,
                            void* actor, void* entity_system, bool owner) {
  auto entity = get_entity(entity_system, engine_id);
  if (!actor || !entity ||
      *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 56) != entity) {
    log("BODY SNAPSHOT row sample=%llu player=%u owner=%u available=0", sample, player,
        unsigned(owner));
    return;
  }
  auto soul = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 1640);
  bool tracked = owner && actor == local_combat_actor.load() && player == local_network_id.load() &&
                 soul == player_command_binding.soul;
  if (!owner) {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    const auto identity = remote_combat_souls.find(soul);
    tracked = identity != remote_combat_souls.end() && identity->second == player;
  }
  const auto state = kcds::runtime::knockout::observe_body(soul, tracked ? soul : nullptr,
                                                           native_soul_is_unconscious);
  if (state == kcds::runtime::knockout::BodyState::Unavailable) {
    log("BODY SNAPSHOT row sample=%llu player=%u owner=%u available=0", sample, player,
        unsigned(owner));
    return;
  }
  const auto physics = kcds::runtime::body_state::read_physics(actor);
  const float health = *reinterpret_cast<const float*>(soul + 1800);
  const auto movement = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 632);
  log("BODY SNAPSHOT row sample=%llu player=%u owner=%u available=1 hp=%.6f native_ko=%u "
      "profile=%u action=%u tick=%llu",
      sample, player, unsigned(owner), health,
      unsigned(state == kcds::runtime::knockout::BodyState::KnockedOut), unsigned(physics.profile),
      native_action_type(movement), GetTickCount64());
}

void tick_native_body_snapshot(void* entity_system, void* local_actor, unsigned local_id,
                               core::EntityId owner, std::uint32_t revision) {
  if (!signaled(body_snapshot_event)) {
    return;
  }
  // A kit requests explicit samples; production and idle test frames perform no native reads.
  constexpr std::size_t kMaxRemoteSamples = 16;
  static std::uint64_t sample = 0;
  ++sample;
  const auto count = std::min(remotes.size(), kMaxRemoteSamples);
  log("BODY SNAPSHOT begin sample=%llu pid=%lu revision=%u owner=%u rows=%zu truncated=%u "
      "tick=%llu",
      sample, GetCurrentProcessId(), revision, owner, count + 1,
      unsigned(remotes.size() > kMaxRemoteSamples), GetTickCount64());
  log_native_body_sample(sample, owner, local_id, local_actor, entity_system, true);
  std::size_t observed = 0;
  for (const auto& [player, remote] : remotes) {
    if (observed == count) {
      break;
    }
    log_native_body_sample(sample, player, remote.engine_id, remote.actor, entity_system, false);
    ++observed;
  }
  log_character_snapshot(sample, entity_system);
  log("BODY SNAPSHOT end sample=%llu rows=%zu tick=%llu", sample, observed + 1, GetTickCount64());
}

}  // namespace kcds::game_client
