'use strict';

// A player's inventory as a shop for another player (Trade), on the
// game's own shop screen the vendors use (vendors.cjs, Client/runtime/vendors).
//
//   Trade.open(seller, buyer, options?)  -> session id, or 0 (a player gone or not ready, the same
//                                           player twice, an inventory missing, or a tradeOpen
//                                           handler returning false)
//     options { prices?: { item: price }, buys?: false, buyPrices?: { item: price } }
//   Trade.close(session)                 -> false once it ended
//   Trade.activeSession(player)              -> the trade session the player buys or sells in, or 0
//
//   events: tradeOpen(seller, buyer)                          false refuses it
//           tradeItem(seller, buyer, bought, sold, balance)   false refuses the basket
//           tradeDone(seller, buyer, bought, sold, balance)
//           tradeClosed(seller, buyer, reason)                the vendors' reasons
//
// The buyer's screen shows the seller's rows as the shelf (every row but money, quest items and
// the units the seller wears), at the item's price in `prices` or the server's price at the row's
// quality, and the seller's money as the purse. With `buys`, the buyer may sell the seller their
// own rows at `buyPrices` (or the server's price). A basket is checked against both inventories,
// then both sides change at once with reason 'trade' (or nothing does): the bought units and the
// money for them move from the seller to the buyer, sold units and their money the other way. Any
// change of either inventory shows the screen anew; baskets priced against the older one are
// refused. The trade ends when the buyer closes the screen, when either player leaves or is knocked
// out, at Trade.close, and when the resource that opened it stops.

const { lookupItemClass } = require('../inventory/catalog.cjs');
const { metadataOf } = require('../inventory/inventories.cjs');

const MAX_PRICE = 1000000000;
const MAX_LINES = 256;
const MAX_AMOUNT = 1000000;

function isWholeNumber(value, low, high) {
  return Number.isInteger(value) && value >= low && value <= high;
}

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function refusal(code) {
  return { ok: false, code };
}

function moneyClass() {
  return lookupItemClass('money');
}

// A basket's lines from the wire: [[row, units]] -> Map row id -> units, or null.
function linesOf(list) {
  if (!Array.isArray(list) || list.length > MAX_LINES) {
    return null;
  }
  const lines = new Map();
  for (const line of list) {
    if (!Array.isArray(line) || line.length !== 2) {
      return null;
    }
    const [row, units] = line;
    if (typeof row !== 'string' || row === '' || !isWholeNumber(units, 1, MAX_AMOUNT)) {
      return null;
    }
    lines.set(row, (lines.get(row) ?? 0) + units);
  }
  return lines;
}

// { item: price } -> Map class id -> price; throws for unusable entries.
function priceList(list, name) {
  const prices = new Map();
  if (list === undefined) {
    return prices;
  }
  if (!isObject(list)) {
    throw new TypeError(`Trade.open options.${name} is an object of item: price`);
  }
  for (const [item, price] of Object.entries(list)) {
    const itemClass = lookupItemClass(item);
    if (!itemClass) {
      throw new TypeError(`Trade.open options.${name}: ${item} is not an item class`);
    }
    if (!isWholeNumber(price, 0, MAX_PRICE)) {
      throw new RangeError(`Trade.open options.${name}: a price is a whole number to ${MAX_PRICE}`);
    }
    prices.set(itemClass.id, price);
  }
  return prices;
}

function tradeable(row) {
  return row.itemClass !== moneyClass() && !row.itemClass.quest;
}

class PlayerTrades {
  // vendors: the Vendors, whose sessions and shop screen trades use.
  constructor(vendors) {
    this.vendors = vendors;
    this.host = vendors.host;
    // While a basket settles: the inventory changes it makes refresh the screen once, after it.
    this.settling = false;
  }

  open(sellerValue, buyerValue, options) {
    if (options !== undefined && !isObject(options)) {
      throw new TypeError('Trade.open options must be an object');
    }
    const { prices, buys = false, buyPrices } = options ?? {};
    if (typeof buys !== 'boolean') {
      throw new TypeError('Trade.open options.buys must be a boolean');
    }
    const sellPrices = priceList(prices, 'prices');
    const boughtPrices = priceList(buyPrices, 'buyPrices');
    const seller = this.vendors.playerOf(sellerValue);
    const buyer = this.vendors.playerOf(buyerValue);
    if (!seller || !buyer || seller === buyer || !seller.ready || !buyer.ready) {
      return 0;
    }
    const inventories = this.host.playerInventories;
    if (!inventories.entryOf(seller) || !inventories.entryOf(buyer)) {
      return 0;
    }
    if (this.host.emit('tradeOpen', seller, buyer) === false) {
      return 0;
    }
    const vendor = {
      id: 0,
      name: '',
      seller,
      buys,
      sellPrices,
      boughtPrices,
      purse: 0,
      shelf: [],
      buyPrices: new Map(),
      revision: 1,
      nextRow: 1,
      owner: this.vendors.owner(),
    };
    this.refresh(vendor, buyer);
    return this.vendors.openSession(vendor, buyer, null);
  }

  // The shelf, the purse and the buy list from both inventories as they are now.
  refresh(vendor, buyer) {
    const inventories = this.host.playerInventories;
    const sellerInventory = inventories.entryOf(vendor.seller)?.inventory;
    const buyerInventory = inventories.entryOf(buyer)?.inventory;
    const money = moneyClass();
    vendor.shelf = [];
    vendor.purse = 0;
    if (sellerInventory) {
      for (const row of sellerInventory.rows.values()) {
        if (row.itemClass === money) {
          vendor.purse += row.amount;
        }
        const free = row.amount - row.equipped;
        if (!tradeable(row) || free <= 0) {
          continue;
        }
        const price =
          vendor.sellPrices.get(row.itemClass.id) ??
          this.vendors.serverPrice(row.itemClass, metadataOf(row.itemClass, row.props));
        vendor.shelf.push({
          id: row.id,
          itemClass: row.itemClass,
          props: row.props,
          amount: free,
          price,
        });
      }
    }
    vendor.purse = Math.min(vendor.purse, MAX_PRICE);
    vendor.buyPrices = new Map();
    if (vendor.buys && buyerInventory) {
      for (const row of buyerInventory.rows.values()) {
        if (!tradeable(row) || vendor.buyPrices.has(row.itemClass.id)) {
          continue;
        }
        const price =
          vendor.boughtPrices.get(row.itemClass.id) ??
          this.vendors.serverPrice(row.itemClass, undefined);
        vendor.buyPrices.set(row.itemClass.id, { itemClass: row.itemClass, price });
      }
    }
  }

  // A player's inventory changed: every trade they buy or sell in shows it.
  inventoryChanged(player) {
    if (this.settling) {
      return;
    }
    for (const session of this.vendors.sessions.values()) {
      if (
        session.vendor.seller === player ||
        (session.vendor.seller && session.player === player)
      ) {
        this.changed(session);
      }
    }
  }

  changed(session) {
    this.refresh(session.vendor, session.player);
    session.vendor.revision += 1;
    this.vendors.show(session, false);
  }

  // The basket the buyer confirmed. Returns { ok, code }.
  settle(buyer, session, revision, boughtList, soldList) {
    const vendor = session.vendor;
    const seller = vendor.seller;
    const bought = linesOf(boughtList);
    const sold = linesOf(soldList);
    if (!bought || !sold || bought.size + sold.size === 0) {
      return refusal('invalidRequest');
    }
    if (revision !== vendor.revision) {
      return refusal('staleRevision');
    }
    const inventories = this.host.playerInventories;
    const sellerInventory = inventories.entryOf(seller)?.inventory;
    const buyerInventory = inventories.entryOf(buyer)?.inventory;
    if (!sellerInventory || !buyerInventory) {
      return refusal('inventoryUnavailable');
    }
    const money = moneyClass();
    // From the seller: their row -> units, at the shelf's price.
    const fromSeller = new Map();
    const boughtLines = [];
    let cost = 0;
    for (const [id, units] of bought) {
      const row = sellerInventory.rows.get(id);
      const shelved = vendor.shelf.find((candidate) => candidate.id === id);
      if (!row || !shelved || !tradeable(row)) {
        return refusal('unknownItem');
      }
      if (units > row.amount - row.equipped) {
        return refusal('insufficientStock');
      }
      fromSeller.set(row, units);
      cost += units * shelved.price;
      boughtLines.push(this.line(row, units, shelved.price));
    }
    // From the buyer: their row -> units, at the buy list's price.
    if (sold.size > 0 && !vendor.buys) {
      return refusal('notBuying');
    }
    const fromBuyer = new Map();
    const soldLines = [];
    let pay = 0;
    for (const [id, units] of sold) {
      const row = buyerInventory.rows.get(id);
      if (!row) {
        return refusal('unknownItem');
      }
      const listed = vendor.buyPrices.get(row.itemClass.id);
      if (!listed || !tradeable(row)) {
        return refusal('notBought');
      }
      if (units > row.amount - row.equipped) {
        return refusal('insufficientItems');
      }
      fromBuyer.set(row, units);
      pay += units * listed.price;
      soldLines.push(this.line(row, units, listed.price));
    }
    // What the buyer gets in money (negative: what they pay).
    const balance = pay - cost;
    if (Math.abs(balance) > MAX_AMOUNT) {
      return refusal('inventoryCapacity');
    }
    if (this.host.emit('tradeItem', seller, buyer, boughtLines, soldLines, balance) === false) {
      return refusal('refused');
    }
    const units = (taken) =>
      [...taken].map(([row, amount]) => ({ itemClass: row.itemClass, props: row.props, amount }));
    const buyerPlan = buyerInventory.planSettle(fromBuyer, money, balance, units(fromSeller));
    if (buyerPlan.code) {
      return refusal(buyerPlan.code);
    }
    const sellerPlan = sellerInventory.planSettle(fromSeller, money, -balance, units(fromBuyer));
    if (sellerPlan.code) {
      return refusal(`seller${sellerPlan.code[0].toUpperCase()}${sellerPlan.code.slice(1)}`);
    }
    this.settling = true;
    try {
      const buyerChange = buyerInventory.commitSettle(buyerPlan).change;
      const sellerChange = sellerInventory.commitSettle(sellerPlan).change;
      inventories.record(buyer, buyerChange);
      inventories.record(seller, sellerChange);
      inventories.announce(buyer, buyerChange);
      inventories.announce(seller, sellerChange);
    } finally {
      this.settling = false;
    }
    // Every trade either player is in shows the inventories as they are now, this one too.
    this.inventoryChanged(seller);
    this.inventoryChanged(buyer);
    this.host.emit('tradeDone', seller, buyer, boughtLines, soldLines, balance);
    return { ok: true, code: '' };
  }

  // What a basket on another player's shelf would move, for Inventory.beforeCommit: kind 'trade'
  // with the `seller`, lines bought from them and sold to them at the screen's prices, and `money`
  // the buyer gets (negative: pays). Null when a row is not there.
  describe(buyer, session, revision, boughtList, soldList) {
    const vendor = session.vendor;
    const seller = vendor.seller;
    const bought = linesOf(boughtList);
    const sold = linesOf(soldList);
    const inventories = this.host.playerInventories;
    const sellerInventory = inventories.entryOf(seller)?.inventory;
    const buyerInventory = inventories.entryOf(buyer)?.inventory;
    if (!bought || !sold || revision !== vendor.revision || !sellerInventory || !buyerInventory) {
      return null;
    }
    const lines = [];
    let money = 0;
    for (const [id, units] of bought) {
      const row = sellerInventory.rows.get(id);
      const shelved = vendor.shelf.find((candidate) => candidate.id === id);
      if (!row || !shelved) {
        return null;
      }
      lines.push({
        ...this.line(row, units, shelved.price),
        row: id,
        from: 'seller',
        to: 'player',
      });
      money -= units * shelved.price;
    }
    for (const [id, units] of sold) {
      const row = buyerInventory.rows.get(id);
      const listed = row ? vendor.buyPrices.get(row.itemClass.id) : undefined;
      if (!row || !listed) {
        return null;
      }
      lines.push({ ...this.line(row, units, listed.price), row: id, from: 'player', to: 'seller' });
      money += units * listed.price;
    }
    return { kind: 'trade', seller, money, lines };
  }

  line(row, amount, price) {
    return {
      metadata: metadataOf(row.itemClass, row.props),
      item: row.itemClass.id,
      name: row.itemClass.name,
      amount,
      price,
    };
  }

  activeSession(playerValue) {
    const player = this.vendors.playerOf(playerValue);
    if (!player) {
      return 0;
    }
    for (const session of this.vendors.sessions.values()) {
      if (
        session.vendor.seller &&
        (session.player === player || session.vendor.seller === player)
      ) {
        return session.id;
      }
    }
    return 0;
  }

  // The trades a player sells in end with them (they left or were knocked out).
  sellerGone(player, reason) {
    for (const session of [...this.vendors.sessions.values()]) {
      if (session.vendor.seller === player) {
        this.vendors.end(session, reason, true);
      }
    }
  }

  api() {
    const self = this;
    return Object.freeze({
      open: (seller, buyer, options) => self.open(seller, buyer, options),
      close: (sessionId) => {
        const session = self.vendors.sessions.get(sessionId);
        return Boolean(session?.vendor.seller) && self.vendors.close(sessionId);
      },
      activeSession: (player) => self.activeSession(player),
    });
  }
}

module.exports = { PlayerTrades };
