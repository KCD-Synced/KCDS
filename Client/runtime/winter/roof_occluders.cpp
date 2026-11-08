#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "roof_occluders.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "../level/level_edits.hpp"
#include "MinHook.h"
#include "snow_stage.hpp"

namespace kcds::runtime::winter::roof_occluders {
namespace {

// The update's code: the call of its rain check, and the RIP-relative loads of the renderer and
// of the per-thread lists (each instruction 7 bytes, its displacement after 3).
constexpr std::size_t kRainCheckCall = 23;
constexpr std::size_t kRendererLoad = 36;
constexpr std::size_t kListsLoad = 43;
// The renderer's thread whose lists the update reads (+ 11940); the lists of one thread are 190
// qwords apart, the occluders' begin and end at qwords 125 and 126 of them.
constexpr std::size_t kRendererThread = 11940;
constexpr std::size_t kThreadQwords = 190;
constexpr std::size_t kOccluderBegin = 125;
// A brush's mesh (CStatObj, + 152) and that mesh's render mesh (+ 208).
constexpr std::size_t kBrushMesh = 152;
constexpr std::size_t kRenderMesh = 208;
// How far around the camera buildings occlude (metres), how many at most, and how often the list
// is made again.
constexpr float kReach = 40.0f;
constexpr std::size_t kMostOccluders = 512;
constexpr auto kRefresh = std::chrono::milliseconds(500);
// How far a building is drawn below its place in the map (metres).
constexpr float kSink = 0.6f;

// One occluder as the update reads it: a render mesh and its world matrix (3x4 by rows).
struct Occluder {
  void* mesh;
  float matrix[12];
};
static_assert(sizeof(Occluder) == 56, "the update steps 56 bytes per occluder");

using Gate = std::int64_t(__fastcall*)(void* stage);
using RainCheck = bool(__fastcall*)(void* stage);

Logger log_line = nullptr;
Gate original_gate = nullptr;
RainCheck original_rain_check = nullptr;
void* const* renderer = nullptr;
std::uint64_t* lists = nullptr;

std::mutex wanted_mutex;
std::vector<Occluder> wanted;
std::chrono::steady_clock::time_point next_refresh{};
std::atomic<bool> logged_first{false};
std::atomic<bool> logged_gate{false};
std::atomic<bool> has_occluders{false};
std::size_t logged_count = SIZE_MAX;
// The update runs on the render thread: its own copy of the occluders for the call.
thread_local std::vector<Occluder> drawn;
thread_local bool in_gate = false;

void* target(void* code, std::size_t at, std::size_t length) {
  std::int32_t displacement = 0;
  std::memcpy(&displacement, static_cast<std::uint8_t*>(code) + at + length - 4,
              sizeof(displacement));
  return static_cast<std::uint8_t*>(code) + at + length + displacement;
}

bool __fastcall hooked_rain_check(void* stage) {
  if (in_gate) {
    return false;
  }
  return original_rain_check(stage);
}

std::int64_t __fastcall hooked_gate(void* stage) {
  if (log_line && !logged_gate.exchange(true)) {
    log_line("WINTER roof occlusion: the map's update runs (snow %d)", snow::active() ? 1 : 0);
  }
  if (!snow::active() || !renderer || !*renderer || !lists) {
    return original_gate(stage);
  }
  {
    std::lock_guard<std::mutex> lock(wanted_mutex);
    drawn = wanted;
  }
  if (drawn.empty()) {
    return original_gate(stage);
  }
  const std::uint32_t thread = *reinterpret_cast<const std::uint32_t*>(
      static_cast<std::uint8_t*>(*renderer) + kRendererThread);
  if (thread > 1) {
    return original_gate(stage);
  }
  std::uint64_t* list = lists + kThreadQwords * thread + kOccluderBegin;
  const std::uint64_t saved[4] = {list[0], list[1], list[2], list[3]};
  list[0] = reinterpret_cast<std::uint64_t>(drawn.data());
  list[1] = reinterpret_cast<std::uint64_t>(drawn.data() + drawn.size());
  list[2] = list[1];
  // The update draws only while this word is set; it only tests it.
  if (!list[3]) {
    list[3] = 1;
  }
  in_gate = true;
  const std::int64_t result = original_gate(stage);
  in_gate = false;
  list[0] = saved[0];
  list[1] = saved[1];
  list[2] = saved[2];
  list[3] = saved[3];
  if (log_line && !logged_first.exchange(true)) {
    log_line("WINTER roof occlusion: %zu buildings drawn into the map (word was %llu)",
             drawn.size(), static_cast<unsigned long long>(saved[3]));
  }
  return result;
}

}  // namespace

bool install(void* gate, Logger log) {
  log_line = log;
  if (!gate) {
    return false;
  }
  void* rain_check = target(gate, kRainCheckCall, 5);
  renderer = static_cast<void* const*>(target(gate, kRendererLoad, 7));
  lists = static_cast<std::uint64_t*>(target(gate, kListsLoad, 7));
  return MH_CreateHook(rain_check, reinterpret_cast<void*>(&hooked_rain_check),
                       reinterpret_cast<void**>(&original_rain_check)) == MH_OK &&
         MH_EnableHook(rain_check) == MH_OK &&
         MH_CreateHook(gate, reinterpret_cast<void*>(&hooked_gate),
                       reinterpret_cast<void**>(&original_gate)) == MH_OK &&
         MH_EnableHook(gate) == MH_OK;
}

void update(const float camera[3]) {
  const auto now = std::chrono::steady_clock::now();
  if (now < next_refresh) {
    return;
  }
  next_refresh = now + kRefresh;
  std::vector<Occluder> found;
  if (snow::active()) {
    for (const level::LevelBrush& brush : level::brushes_near(camera, kReach)) {
      if (brush.hidden || !brush.brush || found.size() >= kMostOccluders) {
        continue;
      }
      void* mesh_object =
          *reinterpret_cast<void* const*>(static_cast<std::uint8_t*>(brush.brush) + kBrushMesh);
      void* render_mesh = mesh_object ? *reinterpret_cast<void* const*>(
                                            static_cast<std::uint8_t*>(mesh_object) + kRenderMesh)
                                      : nullptr;
      if (!render_mesh) {
        continue;
      }
      Occluder occluder{render_mesh, {}};
      std::memcpy(occluder.matrix, brush.matrix, sizeof(occluder.matrix));
      // A little lower in the map than it stands: its own top lies above it and keeps its snow,
      // the ground under it does not.
      occluder.matrix[11] -= kSink;
      found.push_back(occluder);
    }
  }
  const bool empty_changed = found.empty() != (logged_count == 0) || logged_count == SIZE_MAX;
  if (log_line && empty_changed) {
    logged_count = found.size();
    log_line("WINTER roof occlusion: %zu buildings around the camera", found.size());
  }
  has_occluders.store(original_gate && !found.empty());
  std::lock_guard<std::mutex> lock(wanted_mutex);
  wanted = std::move(found);
}

bool occluding() {
  return has_occluders.load();
}

}  // namespace kcds::runtime::winter::roof_occluders
