#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/gate_series.hpp"

// The levels' gates, the same for every player. The game shows each as one of a
// series of level layers (open, closed, barricaded, ...) and changes them only in its quests;
// here every player's game shows the gate in the server's state and moves it there smoothly:
//   - the animated gates (kGates: the Malesov drawbridge and the Suchdol portcullis) with their
//     own animated model over the clip's length;
//   - the static series (kGateSeries, generated from the levels) with copies of their leaves, each
//     moved from its place in one state to its place in the next, or faded in or out.
//
//   GATE : server -> a ready player's client, on the control channel (0): the gate (its number
//          below), its state, and how long ago the move into it began (a late joiner, or a
//          player whose world was not ready, starts the move part way or sees it finished).
//          Client/runtime/gates/ shows it.
namespace kcd2s::integration {

inline constexpr std::uint8_t kGateTag = 81;
// A move older than this is over; the field stops there.
inline constexpr std::uint32_t kMaxGateElapsedMs = 600000;

struct GateModel {
  std::string_view key;    // the server API's name of the gate
  std::string_view map;    // the level it stands in (world config map)
  std::string_view model;  // the animated character the level's animated layer shows
  std::string_view clip;   // the clip that closes the gate, played forwards
  float seconds;           // its length
  // The clip that opens it, played forwards; none: the closing clip played backwards.
  std::string_view open_clip;
  float open_seconds;  // the length of the opening (that clip's, or the closing one's)
  // How far into a move (0 to 1) the gate blocks the way: closing, from this point on; opening,
  // until this point.
  float closing_blocks_from;
  float opening_blocks_until;
  std::array<float, 3> position;
  std::array<float, 4> rotation;  // x, y, z, w (the level's Rotate is w, x, y, z)
  // The static model the level shows in the open state at the same place (a brush: its mesh and
  // the position the level edit finds it by). It is hidden as soon as the world loads, for good:
  // the animated model shows the gate in every state.
  std::string_view open_mesh;
  std::array<float, 3> open_mesh_position;
  // The game's static model of the closed gate at the same place, when it has one; none: a box.
  std::string_view closed_mesh;
  // The animated model has no collision; the gate's own static models give it, unseen: the open
  // model while the way is free and the closed model while the gate blocks it (both placed and
  // turned as the animated one). A gate without a closed model blocks with a box in the model's
  // own axes, its centre and size in metres as the closed pose's bounds read in the game, made
  // thicker so a player pressed against the gate does not see through it.
  std::array<float, 3> blocker_center;
  std::array<float, 3> blocker_size;
};

// From the level's own layers (Data/Levels/kutnohorsko/level.pak, read):
// series_drawbridge/drawbridge_animated and series_portcullis/animated; the open meshes are
// brushes of the level's object tree (terrain/terrain.dat), placed as the game shows them.
inline constexpr std::array<GateModel, 2> kGates = {{
    {"malesovDrawbridge",
     "kutnohorsko",
     "objects/manmade/structures/defensive/fortress/malesov/malesov_drawbridge.cdf",
     "malesov_drawbridge_lifting",
     7.733f,
     "",
     7.733f,
     0.5f,
     0.5f,
     {469.4846f, 371.7452f, 111.9601f},
     {-0.005947837f, -0.002514912f, -0.3894373f, -0.9210303f},
     "objects/manmade/structures/defensive/fortress/malesov/malesov_drawbridge_opened.cgf",
     {469.471f, 371.745f, 111.964f},
     "",
     {4.2f, -8.635f, 0.78f},
     {1.8f, 3.09f, 5.16f}},
    {"suchdolPortcullis",
     "kutnohorsko",
     "objects/manmade/structures/defensive/fortress/suchdol/suchdol_fortres_gate_bars.cdf",
     "suchdol_fortres_gate_bars_close",
     6.967f,
     "suchdol_fortres_gate_bars_open",
     17.033f,
     0.05f,
     0.5f,
     {751.4498f, 3354.923f, 142.0204f},
     {0.0f, 0.0f, 0.009534002f, 0.9999545f},
     "objects/manmade/structures/defensive/fortress/suchdol/suchdol_fortres_gate_bars_opened.cgf",
     {751.4498f, 3354.923f, 142.0204f},
     "objects/manmade/structures/defensive/fortress/suchdol/suchdol_fortres_gate_bars_closed.cgf",
     {10.35f, 0.06f, 1.93f},
     {1.48f, 3.36f, 4.53f}},
}};

// The gates are numbered: kGates first, then the static series (gate_series.hpp, generated).
inline constexpr std::size_t kGateCount = kGates.size() + kGateSeries.size();
// The two states of an animated gate.
inline constexpr std::uint8_t kGateClosed = 0;
inline constexpr std::uint8_t kGateOpen = 1;

inline std::size_t gate_state_count(std::uint32_t gate) {
  if (gate < kGates.size()) {
    return 2;
  }
  return gate < kGateCount ? kGateSeries[gate - kGates.size()].state_count : 0;
}

struct GateState {
  std::uint32_t gate = 0;          // its number (above)
  std::uint8_t state = kGateOpen;  // its state's index (an animated gate: kGateClosed, kGateOpen)
  std::uint32_t elapsed_ms = 0;    // since the gate began to move into it
  // A series with a bar (GateSeries::bar_state): the bar is out of its brackets in that state (a
  // resource took it out, or closed the gate without it), and since when it is on its way.
  bool bar_out = false;
  std::uint32_t bar_elapsed_ms = 0;

  bool open() const {
    return state == kGateOpen;
  }

  friend bool operator==(const GateState& a, const GateState& b) {
    return a.gate == b.gate && a.state == b.state && a.elapsed_ms == b.elapsed_ms &&
           a.bar_out == b.bar_out && a.bar_elapsed_ms == b.bar_elapsed_ms;
  }
};

// GATE: [tag][varuint gate][u8 state][varuint elapsed ms][u8 bar out][varuint bar elapsed ms];
// empty for a gate or state that is not in the catalogs.
inline std::vector<std::uint8_t> encode_gate(const GateState& state) {
  if (state.gate >= kGateCount || state.state >= gate_state_count(state.gate)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kGateTag, 8);
  w.write_varuint(state.gate);
  w.write_bits(state.state, 8);
  w.write_varuint(state.elapsed_ms < kMaxGateElapsedMs ? state.elapsed_ms : kMaxGateElapsedMs);
  w.write_bits(state.bar_out ? 1 : 0, 8);
  w.write_varuint(state.bar_elapsed_ms < kMaxGateElapsedMs ? state.bar_elapsed_ms
                                                           : kMaxGateElapsedMs);
  return w.bytes();
}

inline bool decode_gate(const std::vector<std::uint8_t>& p, GateState& state) {
  if (p.empty() || p[0] != kGateTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t gate = r.read_varuint();
  const std::uint64_t index = r.read_bits(8);
  const std::uint64_t elapsed = r.read_varuint();
  const std::uint64_t bar_out = r.read_bits(8);
  const std::uint64_t bar_elapsed = r.read_varuint();
  if (r.overrun() || gate >= kGateCount || index >= gate_state_count(std::uint32_t(gate)) ||
      elapsed > kMaxGateElapsedMs || bar_out > 1 || bar_elapsed > kMaxGateElapsedMs) {
    return false;
  }
  state.gate = std::uint32_t(gate);
  state.state = std::uint8_t(index);
  state.elapsed_ms = std::uint32_t(elapsed);
  state.bar_out = bar_out == 1;
  state.bar_elapsed_ms = std::uint32_t(bar_elapsed);
  return true;
}

}  // namespace kcd2s::integration
