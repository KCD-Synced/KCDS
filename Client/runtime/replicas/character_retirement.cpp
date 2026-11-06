#include "../core/client_internal.hpp"

#include "character_retirement.hpp"

#include "../core/frame.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "retirement_fence.hpp"

namespace kcds::game_client {
namespace {
kcds::runtime::replicas::RetirementFence retiring_characters;
std::size_t admitted_characters = 0;
}  // namespace

void record_character_created(std::uint32_t engine_id, void* entity) {
  retiring_characters.created(engine_id, entity);
}

void record_character_retirement(void* entity_system, std::uint32_t engine_id) {
  if (entity_system) {
    retiring_characters.observe_system(entity_system);
    retiring_characters.retire(engine_id, get_entity(entity_system, engine_id));
  }
}

void begin_character_frame(const Frame& frame) {
  admitted_characters = 0;
  for (const auto& [id, entity] : frame.entities) {
    if (id != frame.own && (entity.type == core::type_id("NetworkPlayer") ||
                            entity.type == core::type_id("NetworkNpc") ||
                            entity.type == core::type_id("NetworkHorse"))) {
      ++admitted_characters;
    }
  }
  const auto previous = retiring_characters.pending();
  retiring_characters.observe_system(frame.entity_system);
  if (frame.entity_system) {
    retiring_characters.refresh(
        [&frame](std::uint32_t id) { return get_entity(frame.entity_system, id); });
  }
  if (previous != retiring_characters.pending()) {
    log("CHARACTER native allocations active=%zu retiring=%zu admitted=%zu",
        retiring_characters.active(), retiring_characters.pending(), admitted_characters);
  }
}

bool character_spawns_ready() {
  return retiring_characters.can_spawn(admitted_characters);
}

CharacterAllocationCounts character_allocation_counts() {
  return {admitted_characters, retiring_characters.active(), retiring_characters.pending()};
}

}  // namespace kcds::game_client
