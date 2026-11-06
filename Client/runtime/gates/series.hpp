// The levels' static gate series as the server has them (kGateSeries), WHGame 1.5.5.
//
// A series is a folder of level layers, one per state (closed, open, barricaded, ...), of which
// the game shows one; its leaves are brushes of the level's object tree (each with its layer id,
// level/level_edits.hpp brushes_near) and static mesh entities (by EntityGuid). When the world
// is ready the module finds every leaf of every state around the series, takes the shown ones out
// for good and stands its own copy of each leaf in its place: a static mesh entity with the leaf's
// own collision (props/prop_entity.hpp spawn_solid_mesh, physicalized static), the copies of the
// level's current state shown, the others hidden. A series none of whose states the level shows
// is left as the level has it until the server sets one.
//
// A move to another state takes kSeriesSeconds: a leaf with the same mesh in both states (a gate
// leaf swinging on its hinge) travels from its place in the one to its place in the other, turned
// smoothly (slerp). A leaf of only one of the states (a latch bar) is lifted out of its brackets
// and slid out along the gate's line (from its two hinges) before the leaves swing open, and
// slid in and dropped after they swing shut; it comes and goes inside the post's socket at the
// far end of its way. Without travelling leaves to show the line it fades out or in. Every player's
// copy moves the same way from when the move began, so a late joiner sees it part way. Engine
// thread.
#pragma once

#include <string_view>
#include <vector>

#include "kcd2s/integration/gates.hpp"

namespace kcds::runtime::gates::series {

using Logger = void (*)(const char* format, ...);

// The length of a move from one state to another, as the server counts it.
inline constexpr float kSeriesSeconds = 4.0f;

void install(Logger logger);

// Every frame in a ready world of that map with the server's states of series gates (gate numbers
// from kGates.size() on), oldest first.
void frame(std::string_view map, const std::vector<kcd2s::integration::GateState>& states);

// The world is left: its copies went with it.
void reset();

}  // namespace kcds::runtime::gates::series
