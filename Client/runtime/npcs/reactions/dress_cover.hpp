// A woman player in a dress is not naked to the game's NPCs, WHGame 1.5.5.
//
// The game's NPCs remark on a player who wears nothing on the body or nothing on the legs
// (Scripts AI/crime/checkPlayerConspicuousness.xml: an armor type of the torso and one of the legs,
// hose and the like, both worn). A woman's dress is an armor type of its own, so a woman in a dress
// was naked to them until she put on men's hose. The check is skipped for an NPC that has the
// entity context speech_disableNakedPlayerReactionsForNPC, which the game's own rules give some
// NPCs (Libs/Storm/contexts/contexts.xml).
//
// While the local player is a woman, the living bodies near her (the NPCs) get that context as the
// game's context manager counts an entity's contexts: the context's id by its name, the count
// raised for the body's entity and the manager's change handed out (its flush, as the game does it
// when it is not holding changes back). A body that leaves the reach, or every one once she is a
// man again or the world is left, has it taken away. Limit: a woman with nothing on gets no remark
// either.
//
// Engine thread.
#pragma once

namespace kcds::runtime::npcs::reactions::dress_cover {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* manager = nullptr;     // the context manager's singleton
  void* context_id = nullptr;  // (unused, &id, &name): a context's id by its name, a CryString
  void* add = nullptr;         // (manager, entity, id, 6): raises the entity's count of it
  void* remove = nullptr;      // (manager, entity, id, 6): lowers it
  void* flush = nullptr;       // (manager): hands the changes out
  void* has = nullptr;         // (manager, entity, &name): whether the entity has the context
  void* string_from_text = nullptr;
  void* string_free = nullptr;
};

// False when a native is missing (nothing is done then).
bool install(const Natives& natives, Logger logger);

// Each frame in a world: the local player's entity and whether the player is a woman.
void tick(void* local_entity, bool woman);

// The world is left: the bodies that had the context went with it.
void reset();

}  // namespace kcds::runtime::npcs::reactions::dress_cover
