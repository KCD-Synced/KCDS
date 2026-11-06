#include "held_weapon.hpp"

#include "../core/client_internal.hpp"
#include "../core/natives.hpp"
#include "../ground/ground_items.hpp"
#include "drawn_weapons.hpp"

namespace kcds::game_client {

// The skeleton of the weapon an actor holds: the item in its left hand (a bow), else in its
// right (a crossbow, a hand cannon), whose entity has a character of its own; null for none.
void* held_weapon_skeleton(void* actor, void* entity_system) {
  if (!actor || !entity_system) {
    return nullptr;
  }
  for (const bool left : {true, false}) {
    void* item = kcds::runtime::weapons::item_in_hand(actor, left);
    const std::uint32_t id = item ? kcds::runtime::ground::entity_of(item) : 0;
    void* entity = id ? get_entity(entity_system, id) : nullptr;
    void* character =
        entity ? slot<void*(__fastcall*)(void*, unsigned)>(entity, 101)(entity, 0) : nullptr;
    void* skeleton = character ? slot<void*(__fastcall*)(void*)>(character, 5)(character) : nullptr;
    if (skeleton) {
      return skeleton;
    }
  }
  return nullptr;
}

}  // namespace kcds::game_client
