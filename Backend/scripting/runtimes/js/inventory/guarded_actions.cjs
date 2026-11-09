'use strict';

// The players' own actions on inventories, as the bridge reports them, each behind
// Inventory.beforeCommit (commit_guards.cjs): what it would move, how it commits, and what the
// player's game is told when the resources refuse it. A key names an action that the same report
// sent again would repeat (the same use, stack or basket); actions without one can rightly come
// twice, such as dropping one unit of a row and then another.
const ACTIONS = {
  inventoryConsume: (host, player, e) => ({
    key: `use:${e.request}`,
    describe: () => host.playerInventories.describeUse(player, e, false),
    commit: () => host.playerInventories.consume(player, e),
    refuse: () => host.sendConsumeVerdict(player, e.request, 'refused'),
  }),
  inventoryPotConsume: (host, player, e) => ({
    key: `use:${e.request}`,
    describe: () => host.playerInventories.describeUse(player, e, true),
    commit: () => host.playerInventories.consumePot(player, e),
    refuse: () => host.sendConsumeVerdict(player, e.request, 'refused'),
  }),
  // The game kept the units.
  inventoryDrop: (host, player, e) => ({
    key: null,
    describe: () => host.groundItems.describeDrop(player, e.row, e.amount),
    commit: () => host.groundItems.drop(player, e.row, e.amount),
    refuse: () => {},
  }),
  // The game left the stack lying.
  groundItemPickup: (host, player, e) => ({
    key: `pickup:${e.stack}`,
    describe: () => host.groundItems.describePickup(player, e.stack),
    commit: () => host.groundItems.pickup(player, e.stack),
    refuse: () => {},
  }),
  // The screen keeps the basket.
  vendorTrade: (host, player, e) => ({
    key: `trade:${e.session}:${e.revision}`,
    describe: () => host.vendors.describeTrade(player, e.session, e.revision, e.bought, e.sold),
    commit: () => host.vendors.trade(player, e.session, e.revision, e.bought, e.sold),
    refuse: () => {},
  }),
  stashTake: (host, player, e) => ({
    key: null,
    describe: () => host.stashes.describeMove(player, e.stash, e.row, e.amount, false),
    commit: () => host.stashes.take(player, e.stash, e.row, e.amount),
    refuse: () => host.stashes.refuseMove(player, e.stash),
  }),
  stashPut: (host, player, e) => ({
    key: null,
    describe: () => host.stashes.describeMove(player, e.stash, e.row, e.amount, true),
    commit: () => host.stashes.put(player, e.stash, e.row, e.amount),
    refuse: () => host.stashes.refuseMove(player, e.stash),
  }),
  horseGearEquip: (host, player, e) => {
    const piece = { row: e.row, fromBags: e.fromBags === true };
    return {
      key: null,
      describe: () => host.describeHorseGear(player, e.horse, piece),
      commit: () => host.playerHorseGear(player, e.horse, piece),
      refuse: () => {},
    };
  },
  horseGearRemove: (host, player, e) => {
    const piece = { item: e.item };
    return {
      key: null,
      describe: () => host.describeHorseGear(player, e.horse, piece),
      commit: () => host.playerHorseGear(player, e.horse, piece),
      refuse: () => {},
    };
  },
};

// Runs the bridge's report `e` when it is one of these actions; false for any other op.
function runGuardedAction(host, e) {
  const make = Object.hasOwn(ACTIONS, e.op) ? ACTIONS[e.op] : null;
  if (!make) {
    return false;
  }
  const player = host.players.get(e.id);
  const { key, describe, commit, refuse } = make(host, player, e);
  host.commitGuards.run(player, key, describe, commit, refuse);
  return true;
}

module.exports = { runGuardedAction };
