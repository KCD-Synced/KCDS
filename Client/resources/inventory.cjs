'use strict';

// The client-only Inventory global: actions in the local player's native
// inventory screen that a resource takes over for chosen items.
//
// whenUsed registers an override by item class (catalog name or GUID, or a type the server registered
// with Items.register: its id or logical GUID, once the game client told the runtime about it,
// { op: 'itemTypes', types: [{ id, item }] }) or by one native instance ({ uid }). The game
// client hears every registration's classes and UIDs
// ({ op: 'inventoryUseOverrides', classes, uids }) and, for an overridden item that the server's
// stock shows, runs no native action and reports the use ({ op: 'inventoryUse', item, uid, id,
// revision, amount, action }), which goes to one handler: the newest one for that instance, else
// the newest one for its class, as the use arrives. Nothing is consumed by itself; the server
// decides.
//
// The screen's actions taken over: 'secondary' (eat, drink, apply, learn: E held), 'primary' (the
// action the hint bar names, Enter or a double click: equip, read, use, apply, pouch; an overridden
// item without one offers Use) and 'activate' (the paper doll's equip toggle: an item moved onto a
// slot or back to the list). Quick-slot actions outside the inventory are not.

const { lookupItemClass } = require('../../Backend/scripting/runtimes/js/inventory/catalog.cjs');

const MAX_REGISTRATIONS = 256;
const UID = /^[0-9]{1,20}$/;

class ResourceInventory {
  // `shipIcon(resource, path)`: has the game client mount a registered type's icon.
  constructor({ send, shipIcon = () => {} }) {
    this.send = send;
    this.shipIcon = shipIcon;
    // [resource, path] of the registered types' own icons.
    this.icons = [];
    this.next = 1;
    // id -> { owner, kind: 'class' | 'uid', key, handler, invoke }
    this.registrations = new Map();
    // The game client starts with nothing overridden.
    this.sent = JSON.stringify({ op: 'inventoryUseOverrides', classes: [], uids: [] });
    // The server's registered item types: id -> logical GUID, and the GUIDs.
    this.customIds = new Map();
    this.customGuids = new Set();
  }

  // The server's registered item types, the whole set; a type's own icon is shipped
  // to the game client.
  receiveItemTypes({ types }) {
    this.customIds.clear();
    this.customGuids.clear();
    this.icons = [];
    for (const type of Array.isArray(types) ? types : []) {
      if (typeof type?.id !== 'string' || typeof type?.item !== 'string') {
        continue;
      }
      const guid = type.item.toLowerCase();
      this.customIds.set(type.id, guid);
      this.customGuids.add(guid);
      const iconFile = typeof type.iconFile === 'string' ? type.iconFile : '';
      const slash = iconFile.indexOf('/');
      if (slash > 0) {
        this.icons.push([iconFile.slice(0, slash), iconFile.slice(slash + 1)]);
      }
    }
    this.shipIcons();
  }

  // Ships the registered types' icons; again once the client has the resources' files (the
  // types can come first), a shipped one is not sent twice.
  shipIcons() {
    for (const [resource, path] of this.icons) {
      try {
        this.shipIcon(resource, path);
      } catch {
        // A resource this client does not have (yet), or a file it does not ship: the class
        // keeps a missing icon until then.
      }
    }
  }

  // The class key of a selector: a catalog class's GUID, else a registered type's GUID.
  classKey(item) {
    const itemClass = lookupItemClass(item);
    if (itemClass) {
      return itemClass.id;
    }
    if (typeof item !== 'string') {
      return null;
    }
    if (this.customIds.has(item)) {
      return this.customIds.get(item);
    }
    const guid = item.toLowerCase();
    return this.customGuids.has(guid) ? guid : null;
  }

  // The Inventory global of one resource; `invoke` runs its callbacks.
  facade(owner, invoke) {
    return Object.freeze({
      whenUsed: (item, handler) => this.whenUsed(owner, invoke, item, handler),
      stopWhenUsed: (id) => this.stopWhenUsed(owner, id),
    });
  }

  whenUsed(owner, invoke, item, handler) {
    if (typeof handler !== 'function') {
      throw new TypeError('whenUsed takes a handler function');
    }
    let match;
    if (item !== null && typeof item === 'object' && typeof item.uid === 'string') {
      if (!UID.test(item.uid)) {
        throw new TypeError(`Not a native item UID: ${item.uid}`);
      }
      match = { kind: 'uid', key: item.uid };
    } else {
      const key = this.classKey(item);
      if (!key) {
        throw new TypeError(`Unknown item class: ${String(item)}`);
      }
      match = { kind: 'class', key };
    }
    if (this.registrations.size >= MAX_REGISTRATIONS) {
      throw new RangeError(`More than ${MAX_REGISTRATIONS} inventory use handlers`);
    }
    const id = this.next;
    this.next += 1;
    this.registrations.set(id, { owner, ...match, handler, invoke });
    this.sync();
    return id;
  }

  stopWhenUsed(owner, id) {
    const registration = this.registrations.get(id);
    if (!registration || registration.owner !== owner) {
      return false;
    }
    this.registrations.delete(id);
    this.sync();
    return true;
  }

  // A stopped resource's handlers go.
  removeOwner(owner) {
    for (const [id, registration] of this.registrations) {
      if (registration.owner === owner) {
        this.registrations.delete(id);
      }
    }
    this.sync();
  }

  // Tells the game client what is overridden, when that changed.
  sync() {
    const classes = new Set();
    const uids = new Set();
    for (const registration of this.registrations.values()) {
      (registration.kind === 'class' ? classes : uids).add(registration.key);
    }
    const message = { op: 'inventoryUseOverrides', classes: [...classes], uids: [...uids] };
    const key = JSON.stringify(message);
    if (key !== this.sent) {
      this.sent = key;
      this.send(message);
    }
  }

  // A use the game client took over: the newest handler for the instance, else for its class.
  receiveUse({ item, uid, id, revision, amount, action }) {
    const newestFirst = [...this.registrations.values()].reverse();
    const registration =
      newestFirst.find((entry) => entry.kind === 'uid' && entry.key === uid) ??
      newestFirst.find((entry) => entry.kind === 'class' && entry.key === item);
    if (!registration) {
      return;
    }
    const use = Object.freeze({ item, uid, id, revision, amount, action });
    registration.invoke(registration.handler, [use]);
  }
}

module.exports = { ResourceInventory };
