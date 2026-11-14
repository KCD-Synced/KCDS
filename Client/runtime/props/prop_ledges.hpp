// Vaultable ledges along placed fences, in the game's own ledge manager (WHGame 1.5.5).
//
// The player vaults over what the ledge manager knows: the level's ledges, baked by the editor
// (the level's whdata LedgeManager: its ledges and their markers). A fence placed as a prop has
// none, so nobody vaulted it. The manager (game services + 0xE0, the game; its + 0x110) keeps:
//
//   + 0   its ledges, 16 bytes each: entity id (+ 0), first marker (+ 4, u16), marker count
//         (+ 6, u16), the two sides' flags (+ 8, + 10, u16), corner end adjust (+ 12, float);
//         their count at + 16
//   + 8   their markers, 28 bytes each: world position (+ 0), facing (+ 12), end or corner (+ 24);
//         their count at + 20
//   + 0x60040  the grid the player's ledge search reads, built from both
//
// How the player's ledge search uses a side's flags (its transition choice): 0x2 kept in the grid,
// 0x8 thin and 0x4 vault (with neither 0x100, 0x80 nor 0x10) give VaultOver, over the fence. The
// player must stand on the side the facing points to, and VaultOver is only taken on a ledge's
// first side (the second, 0x20, only climbs onto it, and the player dropped back). A fence gets
// two one-sided ledges, one each way, each with a marker at each end of the fence's top. KCDS
// appends them after the level's with the arrays' own resize (which frees the old array: the
// content is copied first) and builds the grid again, once a frame when something changed. A level
// loaded again brings its own arrays: the ledges are added again after them.
//
// Engine thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::props::ledges {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*services)() = nullptr;   // the game's services; + 0xE0 the game
  void* resize_ledges = nullptr;   // (manager, count, unused): the ledge array made anew
  void* resize_markers = nullptr;  // (manager, count, unused): the marker array made anew
  void* rebuild_grid = nullptr;    // (manager): the grid built from the arrays
};

// False when a native is missing; fences are then not vaultable.
bool install(const Natives& natives, Logger logger);

// A vaultable ledge from `from` to `to` (the top of a fence, world space) for prop `id`; a new one
// replaces the prop's earlier one.
void set(std::uint32_t id, const float from[3], const float to[3]);

// Prop `id`'s ledge taken away.
void remove(std::uint32_t id);

// Every frame in a world: the manager's arrays and grid made again when the ledges changed, or
// when the level loaded its own again.
void frame();

// The world is left: the ledges are forgotten (the next world's props set theirs).
void reset();

}  // namespace kcds::runtime::props::ledges
