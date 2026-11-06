#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace kcds::runtime::human_overlay {

// WHGame 1.5.5, Windows x64. The integration owner supplies uniquely resolved functions.
// This adapter only accepts the observed human CAF overlay assets; layer ownership and native
// Start/StopAnimation calls remain with the engine-thread integration owner.
struct Natives {
  void* getAnimationSet = nullptr;          // void*(void* character), character v15/v16
  void* entry = nullptr;                    // void*(void* set, int id)
  void* resolveHash = nullptr;              // int64_t(void* set, uint64_t alias), set v8
  void* getSkeleton = nullptr;              // void*(void* character), character v5
  void* const* characterManager = nullptr;  // Unique RIP-resolved qword_185189BE0 slot
};

// Engine thread only, including install. Missing functions disable all lookups. Only function
// addresses are retained; each lookup obtains the current model, set and entry again.
bool install(const Natives& natives);

// Current native lifetime identity, for equality comparisons only. Retired values must never be
// dereferenced. The adapter retains none of these tokens and the network must never carry them.
struct Scope {
  std::uintptr_t character = 0;
  std::uintptr_t model = 0;
  std::uintptr_t animation_set = 0;
  std::uintptr_t entries = 0;
  std::uint32_t count = 0;
  std::uintptr_t manager = 0;
  std::uintptr_t global_cafs = 0;
  std::uint32_t global_caf_count = 0;
  std::uint32_t facial_begin = 0;
  std::uint32_t facial_end = 0;  // Exclusive; the interval loaded from FacialAnimations.img

  bool operator==(const Scope&) const = default;
};

std::optional<Scope> scope(void* skeleton);

// Build only when the current scope changes. Publish an immutable copy or hold the caller's
// synchronization while replacing it. Worker hooks may inspect the cached scalar membership;
// they must not call this adapter or dereference any scope token.
struct Membership {
  Scope scope;
  std::vector<std::uint8_t> clips;

  bool contains(int id) const {
    return id >= 0 && static_cast<std::size_t>(id) < clips.size() && clips[id] != 0;
  }
};

std::optional<Membership> snapshot(void* skeleton);

// Engine thread, under the caller's pak mutation exclusion, immediately before native Start.
// Only a complete resident facial CAF is accepted, selecting the native path without StartRead.
// This performs metadata reads only; it never starts streaming or retries on a timer.
bool ready_for_start(void* skeleton, int id);

// skeleton must be a tracked native skeleton in the current world. Neither arbitrary pointers
// nor process-local clip IDs belong on the wire. Unknown, stale and non-facial identities fail
// closed, including ordinary body CAF assets.
std::optional<std::uint64_t> alias(void* skeleton, int id);
std::optional<int> resolve(void* skeleton, std::uint64_t hash);

}  // namespace kcds::runtime::human_overlay
