#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::game_client {

struct Frame;

// Engine thread only. The frame's admitted remote characters supply allocation slots;
// both active native bodies and deferred retirements occupy them until native destruction.
void begin_character_frame(const Frame& frame);
void record_character_created(std::uint32_t engine_id, void* entity);
// Record before calling RemoveEntity(false).
void record_character_retirement(void* entity_system, std::uint32_t engine_id);
bool character_spawns_ready();
struct CharacterAllocationCounts {
  std::size_t admitted = 0;
  std::size_t active = 0;
  std::size_t retiring = 0;
};
// Read-only diagnostics on the engine thread; does not refresh or release any reservation.
CharacterAllocationCounts character_allocation_counts();
// Session/world reset does not clear retirements. A later refresh proves each body's
// disappearance or observes a different nonnull entity system with a different ID table.

}  // namespace kcds::game_client
