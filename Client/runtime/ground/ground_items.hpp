// Stacks of items lying in the world, made the way the game's own drop makes them
// (WHGame 1.5.5, the inventory screen's drop).
//
// The game's drop moves the units into the world's inventory (the item services' inventory list,
// + 0x100070) and then lays the item there (item, a std::function run on the
// PickableItem entity it spawns, position, rotation), the function being the one that pushes it
// (makes it from an impulse). A stack here is made directly in the world's inventory
// from an ItemDesc with the game's own add and laid with the drop's
// push: the game pushes a dropped item sideways with an impulse as large as its units' weight
// (at least 0.1) in a random direction, so it falls over (a sword dropped upright
// would stand on its tip). Here the direction comes from the stack's id, the same on every
// client; a stack already at rest is laid with no push.
//
// The level's own items (tankards, tools and the rest the level places) lie on every client, each
// its own copy: a pick-up of one is asked of the server first, by the item's key (its class and
// where it lies, kcd2s/integration/world_items.hpp). The game takes it up only when the server
// grants it, with its own pick-up (the item's OnUsed, so the game says what was taken); every other
// client takes the item out of its world, and so does a client that comes into the world later.
//
// Engine thread.
#ifndef KCDS_RUNTIME_GROUND_GROUND_ITEMS_HPP
#define KCDS_RUNTIME_GROUND_GROUND_ITEMS_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/world_items.hpp"

namespace kcds::runtime::ground {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*services)() = nullptr;
  void* item_desc_init = nullptr;  // (desc, guid*, amount, health)
  void* item_desc_free = nullptr;
  void* inventory_add = nullptr;     // (inventory, desc, reason, flag)
  void* inventory_remove = nullptr;  // (inventory, item, reason, amount)
  void* lay = nullptr;               // (item, function, position, rotation)
  void* push_function = nullptr;     // (function, impulse)
  void* item_weight = nullptr;       // (item): its units' weight
  void* function_free = nullptr;     // (std::function destructor)
  // The game's item by handle: (the registry, &handle) on the registry the global
  // points to, at + 0x168 (as asks).
  void* item_by_handle = nullptr;
  void* const* item_registry = nullptr;
  // A pickable's script OnUsed (bind, function handler, user entity id), hooked:
  // a stack this client laid for the server is not taken up by the game; its pick-up is asked of
  // the server instead.
  void* script_on_used = nullptr;
  // The game's drop of an owner's units (item handle, units, ..., &dropped),
  // hooked: units of a row of the server's stock stay; the drop is asked of the server.
  void* drop = nullptr;
  // Where an entity is; false when there is no such entity.
  bool (*entity_position)(std::uint32_t entity, float* position) = nullptr;
  // Where an entity is and its turn (x, y, z, w); false when there is no such entity.
  bool (*entity_pose)(std::uint32_t entity, float* position, float* rotation) = nullptr;
  // Takes an entity's body away, so nothing moves it; false when it has none yet.
  bool (*unphysicalize)(std::uint32_t entity) = nullptr;
  // The game framework (IGameFramework): its item system (vtable + 208) finds the item an
  // entity lays (+ 168), as the script bind does.
  void* (*framework)() = nullptr;
  // Removes an entity (the entity system's RemoveEntity, CEntitySystem vtable slot 19).
  void (*remove_entity)(std::uint32_t entity) = nullptr;
  // Hides an entity or shows it again (CEntity::Hide, slot 63): a level item taken up or turned
  // off stays hidden, since a removed one is made anew by the game.
  void (*hide_entity)(std::uint32_t entity, bool hidden) = nullptr;
  // The game putting an item into the world while it runs (item, ..., flags), which
  // spawns its pickable at once (the level's own items stream in through another path), hooked:
  // while it runs for an item that is not one of this module's stacks, its pickable is refused.
  void* place_in_world = nullptr;
};

bool install(const Natives& natives, Logger logger);

struct Stack {
  std::array<std::uint8_t, 16> item{};  // the class GUID as the game keeps it
  std::uint32_t amount = 1;
  float health = -1.0f;      // -1: the class's own
  std::uint8_t quality = 0;  // 0: worked out from health
  float position[3] = {};
  float rotation[4] = {0, 0, 0, 1};  // x, y, z, w
  // Laid with the drop's push (a stack not yet at rest), its direction from `push_seed`.
  bool push = false;
  std::uint32_t push_seed = 0;
  // Held where it lies without a body, as the game holds an arrow stuck in a target.
  bool pinned = false;
};

// Lays a stack in the world; the game's item, or null when it could not.
void* lay(const Stack& stack);

// The level's items the local player's game would take up since the last call: their keys, for
// the server to rule on (WORLD_PICKUP).
std::vector<std::string> take_world_pickups();

// The server's verdicts on them (key, granted): a granted item is taken up with the game's own
// pick-up.
void rule_world_pickups(const std::vector<std::pair<std::string, bool>>& verdicts);

// The level's items taken up (WORLD_TAKEN): every one of them still lying in this world goes,
// now and as the level streams more in. `full` replaces the list.
void world_taken(bool full, const std::vector<std::string>& keys);

// An entity of the pickable class (PickableItem) was made: the level's item it lays is looked at by
// the sweeps. Any thread.
void level_item_spawned(std::uint32_t entity);

// The server turned the level's own items off (every one goes out of this world, now and as the
// level streams more in, and the game's pick-up of one is refused) or on again, when they come
// back.
void set_level_items_enabled(bool enabled);

// The places a script took the level's items from (WORLD_ITEMS_REMOVED, World.removeLevelItems):
// the level's own items lying inside one, of its class if it names one, are hidden; when the list
// no longer has their place, they come back.
void set_removed_areas(const std::vector<kcd2s::integration::RemovedItemArea>& areas);

// The world changed or the player left it: the list of items taken goes with it.
void reset_world();

// Takes the level's items others took up out of this world (a part of the level that streamed in
// may hold some); call every frame, it looks every two seconds.
void sweep_world();

// Whether a pickable being spawned now is an item the game itself put into the world while it runs
// (such items exist on one client only, so they are refused; a
// server lays what it wants as ground items). The level's own items, the server's stacks, items a
// carry let go and landed projectiles are not refused.
bool refuses_runtime_item();

// Takes a stack the game still holds out of the world.
bool take_away(void* item);

// A stack the server has in this client's view (NetworkGroundItem), by network id.
struct Seen {
  std::uint32_t id = 0;
  Stack stack;
  // Whether the server knows where it came to rest; this client watches the others.
  bool resting = true;
};

// Lays the stacks in view that are not laid yet and takes away the ones that left the view or
// the world; a stack the game no longer holds (the level went) is forgotten. A laid stack the
// server moved (to where it came to rest) is laid again there.
void sync(const std::vector<Seen>& seen);

// Where a stack that was not resting came to rest in this client's world: its game
// entity stopped moving.
struct Rest {
  std::uint32_t id = 0;
  float position[3] = {};
  float rotation[4] = {0, 0, 0, 1};  // x, y, z, w
};

// The rests found since the last call, a few at a time (the server takes a client's reports at a
// limited rate).
std::vector<Rest> take_rests();

// The network id of the stack an item of the game lays, or 0.
std::uint32_t stack_of(void* item);

// Hands the item laid for a stack in view to a carry: the stack is no longer this
// module's to take away, and is not laid again while it stays in view. The item, or null when
// the stack is not laid.
void* claim(std::uint32_t stack);

// Takes back a stack whose carry did not start: its item is this module's again.
void unclaim(std::uint32_t stack, void* item);

// The item a pickable entity lays loose in the world (the level's or the game's own, not a stack
// in view), or null.
void* level_item_of(std::uint32_t entity);

// The id of the entity an item lying in the world is, 0 for none.
std::uint32_t entity_of(void* item);

// Whether an item lies in the world (the world's inventory holds it).
bool lies_in_world(void* item);

// The stacks the local player asked to take up since the last call, by network id.
std::vector<std::uint32_t> take_pickups();

// The units of the server's rows the local player dropped since the last call.
struct Drop {
  std::string row;
  std::uint32_t amount = 0;
};
std::vector<Drop> take_drops();

// Asks the server to drop units of a row of the local player's stock, as the inventory screen's
// drop does (a knocked-out player's weapon let go, knockout/dropped_weapons.hpp).
void ask_drop(const std::string& row, std::uint32_t amount);

// The game's item of a handle (an item's WUID), or null when there is none any more.
void* item_of_handle(std::uint64_t handle);

}  // namespace kcds::runtime::ground

#endif  // KCDS_RUNTIME_GROUND_GROUND_ITEMS_HPP
