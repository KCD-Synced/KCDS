// The status effects on the local player's body: reported to the
// server (BUFFS), the server's orders applied (BUFF_ORDER: Player.addBuff, removeBuff,
// clearBuffs) and its claim kept (Buffs.claim: BUFF_BLOCKED), through the game's own buff
// manager (WHGame 1.5.5):
//
//   the manager     C_BuffManager, from its getter; vtable slot 0 adds a buff
//                   (manager, soul, &GUID, source, &params) and returns the instance, slot 3
//                   removes one instance (manager, &instance id), slot 5 every instance of a buff
//                   (manager, soul, &GUID), slot 6 every buff of a family (manager, soul, &tag)
//   a soul's buffs  soul + 1352 .. + 1360, a vector of instances: + 8 the instance's id (64-bit),
//                   + 16 its definition (+ 0 the GUID, + 32 the class id, + 84 the family or -1)
//   a new instance  the manager's create (manager, &out, soul, &GUID, source,
//                   &params), whatever adds it (an effect, a perk, an item, a script); it looks
//                   the definition up. Hooked: on the local player's soul, a buff
//                   of a class the server claimed is not created unless the server ordered it,
//                   and the client tells the server (playerBuffBlocked).
//   an instance     the create's constructor (manager, &out, soul, definition,
//                   original definition, init parameters, &perk GUID) makes the instance of the
//                   definition's implementation and inits it from that definition (its stat
//                   modifiers from the buff_params text). add_defined() hands it a definition of
//                   the caller's own (custom_buffs.hpp) in the game's add.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/buffs.hpp"

namespace kcds::runtime::buffs {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* manager = nullptr;     // the buff manager's getter
  void* create = nullptr;      // the manager's create, hooked
  void* definition = nullptr;  // a buff's definition by GUID
  void* construct = nullptr;   // the create's constructor; without it no add_defined
};

// False when a native is missing or the hook fails.
bool install(const Natives& natives, Logger logger);

// Every frame: the local player's soul (nullptr outside a server world: the claim and the
// server's instances are dropped).
void tick(void* soul);

// Applies the server's buff orders to the soul.
void apply(void* soul, const std::vector<kcd2s::integration::BuffOrder>& orders);

// A buff on the body, as the report and the client's resource runtime carry it.
struct Carried {
  kcd2s::integration::BuffEntry entry;
  double age = 0;  // seconds since this client first saw it
};

// The soul's status effects (the classes a server can claim) and the buffs the server added.
std::vector<Carried> read(void* soul);

// The claimed buffs the game tried to add since the last call (each at most twice a second).
std::vector<kcd2s::integration::Guid> take_blocked();

// The game's definition of a buff, null when the game has none.
const std::uint8_t* definition_of(const kcd2s::integration::Guid& buff);

// Adds an instance of `definition` to the soul through the game's add of the buff `buff` (whose
// GUID the definition carries). The definition must outlive the instance. The new instance,
// null when none was made.
void* add_defined(void* soul, const kcd2s::integration::Guid& buff, const void* definition);

// Adds the buff to any soul (an NPC's) through the game's add; the server neither
// orders nor sees it. The new instance's id, 0 when none was made.
std::uint64_t add_to_soul(void* soul, const kcd2s::integration::Guid& buff);

// Removes one instance by its id; false when the manager has no such instance.
bool remove_instance(std::uint64_t instance);

// The ids of the instances on the soul.
std::vector<std::uint64_t> instance_ids(void* soul);

}  // namespace kcds::runtime::buffs
