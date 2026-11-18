'use strict';

// Crowns as items: a type registered with Items.register({ crown }) is usable, and using it puts
// that crown on the player (Player.crown) or, when the crown it put on is still worn, takes it
// off. A crown an item put on comes off when the player no longer holds an item of that type
// (dropped, sold, given away). A crown a script set itself (Player.crown) is not touched.

class CrownItems {
  constructor(host) {
    this.host = host;
    // Player -> { item, crown }: the crown worn from an item, by the item's class id.
    this.worn = new WeakMap();
  }

  // A player used a row of a type registered with a crown: on or off. Whether it changed.
  used(player, row, crown) {
    const worn = this.worn.get(player);
    if (worn && worn.crown === crown && player.crown === crown) {
      this.worn.delete(player);
      this.host.setHeadPiece(player, { crown: null });
      return true;
    }
    this.worn.set(player, { item: row.item, crown });
    this.host.setHeadPiece(player, { crown });
    return true;
  }

  // After a change to the player's inventory: a crown worn from an item they no longer hold comes
  // off.
  inventoryChanged(player) {
    const worn = this.worn.get(player);
    if (!worn) {
      return;
    }
    if (player.crown !== worn.crown) {
      this.worn.delete(player);
      return;
    }
    const entry = this.host.playerInventories.entryOf(player);
    const rows = entry ? [...entry.inventory.rows.values()] : [];
    if (rows.some((row) => row.itemClass.id === worn.item)) {
      return;
    }
    this.worn.delete(player);
    this.host.setHeadPiece(player, { crown: null });
  }
}

module.exports = { CrownItems };
