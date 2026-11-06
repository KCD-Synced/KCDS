// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "stash_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "stashes.hpp"

namespace kcds::game_client {

void install_stash(const kcds::runtime::book::Natives& natives) {
  kcds::runtime::stash::Natives stash;
  stash.services = natives.services;
  stash.local_entity = natives.local_entity;
  stash.open_transfer =
      resolve_unique("OpenItemTransferStore", "40 55 53 56 57 41 54 41 56 48 8D 6C 24 88");
  stash.transfer_closed =
      resolve_unique("ItemTransferClosed", "48 89 5C 24 20 57 48 83 EC 20 8B 91 80 00 00 00");
  stash.transfer_item =
      resolve_unique("ItemTransferItem", "40 53 55 57 48 81 EC 90 00 00 00 48 8B 05 ?? ?? ?? ??");
  stash.inventory_by_id =
      resolve_unique("InventoryById", "48 89 5C 24 08 57 48 83 EC 20 4C 8B 02 48 8B FA 49 8B C0");
  // A script's lock on a container: C_LockBase::SetLocked and the lock
  // of an entity id as the lock script bindings find it, resolved with the pillory's.
  stash.set_locked = lock_set_locked;
  stash.lock_of_entity_id = lock_of_entity_id;
  // The lock-picking minigame's unlock (its own virtual method; it calls
  // SetLocked(lock, 0) itself): a lock pick of a container is reported to the server.
  stash.lockpick_unlock =
      resolve_unique("LockpickUnlock",
                     "48 89 5C 24 08 48 89 6C 24 18 56 57 41 56 48 81 EC 80 00 00 00 0F B6 EA 49 "
                     "8B D9 8B D5 45 8A F0 83 F2 01 48 8B F1");
  log("STASH containers %s",
      kcds::runtime::stash::install(stash, log) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
