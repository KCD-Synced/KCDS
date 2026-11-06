// The client scripts' own prop previews: PropGhost and PropPlacer in client
// resources (Client/resources/previews.cjs). They are the server's previews (ghosts.hpp,
// placer.hpp) with orders from this machine's resource runtime instead of the server:
//
//   { op: 'localGhost', kind: 'create' | 'pose' | 'tint' | 'hidden' | 'destroy', ghost, models,
//     offsets, rotations, position, rotation, scale, tint, hidden }
//   { op: 'localPlacer', kind: 'begin' | 'veto' | 'end' | 'rotate', session, models, offsets,
//     rotations, scale, validTint, invalidTint, range, fallbackDistance, verticalOffset, yaw,
//     alignToSurface, faceAwayFromPlayer, keepOpen, requireSurface, maxSlope, minDistance,
//     maxDistance, vetoed, degrees }
//
// A local ghost's or session's number has kLocalBit set, so it never meets one of the server's.
// The reports of a local session go back to the runtime, never to the server:
//
//   { op: 'localPlacerReport', session, event: 'move' | 'confirm' | 'rejected' | 'cancel' |
//     'end', valid, pose: { position, rotation, normal, distance, onSurface } }
//
// Requests: any thread (handle_runtime_message). Orders: engine thread (take). Reports: kept by
// the engine thread (report), sent by the network thread (pump).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "kcd2s/integration/previews.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::props::local_previews {

inline constexpr std::uint32_t kLocalBit = 0x80000000u;

// A local ghost's box changed (PropGhost.getBounds): sent to the runtime with the next pump as
// { op: 'localGhostBounds', ghost, min, max } (no min and max: none).
void report_bounds(std::uint32_t ghost, bool present, const float min[3], const float max[3]);

inline bool local(std::uint32_t number) {
  return (number & kLocalBit) != 0;
}

// A resource runtime message; false when it is not one of these. Any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// The orders since the last call, in the order they came (engine thread); a `rotate` is applied
// to the running session at once.
void take(std::vector<kcd2s::integration::GhostOrder>& ghosts,
          std::vector<kcd2s::integration::PlacerOrder>& placements);

// A report of a local session, for the runtime (engine thread).
void report(const kcd2s::integration::PlacerReport& report);

// Sends the reports to the runtime (network thread).
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::props::local_previews
