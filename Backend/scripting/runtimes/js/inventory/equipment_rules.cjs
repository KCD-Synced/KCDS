'use strict';

// Which worn pieces go on a body together. The game keeps one piece where two want
// the same place and leaves a piece off when what it goes over or under is missing or in the way,
// and Inventory.set used to answer ok anyway. An Inventory.set that would leave such pieces
// equipped is refused with invalidEquipment instead.
//
// A piece is one unit of an equipped row whose class has a slot (a row of two equipped is two
// pieces). Weapons, ammunition and classes without a slot are not pieces.

const { lookupSlot } = require('./catalog.cjs');

// Plate legs that cover the feet fill this slot too, for a slot that needs it filled (spurs).
const FEET_SLOT = 'boot';

// What stops the pieces of `rows` ({ itemClass, equipped }) from being worn together: a list of
// conflicts, empty when they can be. A conflict is one of
// - samePlace: slots (two or more) sharing one place on the body hold more than one piece. A slot
//   is a place of its own, so two pieces of one slot conflict too (rings as well);
// - needsFilled: the piece in `slot` goes over or under the slot `needs`, which is empty;
// - needsEmpty: the piece in `slot` is not worn while the slot `needs` is filled;
// - feet: boots with plate legs that cover the feet.
function equipmentConflicts(rows) {
  const pieces = new Map();
  let coversFeet = false;
  for (const row of rows) {
    const slot = row.equipped > 0 ? row.itemClass.slot : null;
    if (!lookupSlot(slot)) {
      continue;
    }
    pieces.set(slot, (pieces.get(slot) ?? 0) + row.equipped);
    if (row.itemClass.coversFeet === true) {
      coversFeet = true;
    }
  }
  const conflicts = [];
  const places = new Map();
  for (const [slot, count] of pieces) {
    const place = lookupSlot(slot).place;
    const there = places.get(place) ?? { slots: [], count: 0 };
    there.slots.push(slot);
    there.count += count;
    places.set(place, there);
  }
  for (const there of places.values()) {
    if (there.count > 1) {
      conflicts.push({ rule: 'samePlace', slots: there.slots });
    }
  }
  const filled = (slot) => pieces.has(slot) || (slot === FEET_SLOT && coversFeet);
  for (const slot of pieces.keys()) {
    const { requiresFilled, requiresEmpty } = lookupSlot(slot);
    if (requiresFilled && !filled(requiresFilled)) {
      conflicts.push({ rule: 'needsFilled', slot, needs: requiresFilled });
    }
    if (requiresEmpty && pieces.has(requiresEmpty)) {
      conflicts.push({ rule: 'needsEmpty', slot, needs: requiresEmpty });
    }
  }
  if (coversFeet && pieces.has(FEET_SLOT)) {
    conflicts.push({ rule: 'feet', slot: FEET_SLOT });
  }
  return conflicts;
}

module.exports = { equipmentConflicts };
