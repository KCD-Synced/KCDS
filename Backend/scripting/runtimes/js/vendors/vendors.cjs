'use strict';

// Vendors the server holds (Vendor): a shelf of rows with
// prices, what the vendor pays for the players' items, and a purse, traded on the game's own shop
// screen (Client/runtime/vendors).
//
//   Vendor.create(options?)               -> vendor id; options { name?, purse?, buys?, haggle? }
//   (haggle: true or { tolerance?, rounds? }, see haggle.cjs)
//   Vendor.destroy(vendor)                -> false when there was no such vendor
//   Vendor.setShelf(vendor, rows)         -> rows [{ item, amount, price?, metadata? }], at most 128
//   Vendor.setOffers(vendor, rows)     -> rows [{ item, price? }], at most 128
//   A row without a price takes the server's price of the item when it is set: the item config's
//   or Items.override's price, else the game's own, at the row's quality
//   (Inventory.itemPrice's pristine price); a later override does not reprice a set row.
//   Vendor.fillPurse(vendor, purse | null) -> null: a purse that never runs out
//   Vendor.purseOf(vendor)               -> money units, or null
//   Vendor.open(vendor, player, npc?)     -> session id, or 0 if the vendor or player no longer exists
//   Vendor.close(session)                 -> false once the session ended
//   Vendor.activeSession(player), Vendor.shopOf(session)
//
//   events: vendorTrade(vendor, player, bought, sold, balance, haggled)
//           vendorHaggle(vendor, player, basket, offered): answers an offer (haggle.cjs)
//           vendorClosed(vendor, player, reason): 0 closed by the player, 1 closed by the
//           server, 2 replaced by another vendor, 3 failed to open, 4 player disconnected.
//
// bought and sold are lines { item, name, amount, price, metadata } at the vendor's unit prices;
// balance is the money the player got (negative: paid). After a haggle the vendor agreed to, the
// same basket on the same shelf settles at the agreed price: balance is minus that price and
// haggled is true.
//
// Prices use units of the game's `money` item. The client only shows an estimate:
// pressing Trade sends the basket (shelf rows bought, the player's own rows sold, with the shelf's
// revision). The server prices it again from its own lists and settles it at once: the player's
// side in one commit on their inventory with reason 'trade', the vendor's shelf and purse with it,
// or nothing at all. A basket priced against an older shelf (a setShelf or setOffers since, or
// another player's deal) is refused, as is one the player cannot pay, the purse cannot pay, or the
// player's inventory has no room for. Items sold to the vendor are not put up for sale.
//
// Each player can use one vendor at a time; a new one closes the old one with reason 2. The
// session ends with reason 4 when the player disconnects or their world loads again, and with
// reason 1 when they are knocked out, its NPC dies or goes, the vendor is destroyed or the resource
// that created the vendor or opened the session stops.
//
//   { op: 'vendorShow', id, session, open, buys, npc, revision, purse, money,
//     rows: [[id, class, amount, quality, health, 0]], prices: [per row], buyPrices: [[class, price]] }
//   { op: 'vendorClose', id, session }
//   { op: 'vendorHaggleReply', id, session, round, answer: 1 accept | 2 counter | 3 mock |
//     4 refuse, price, mood: 0 .. 100 }
//   from the client: { op: 'vendorTrade', id, session, revision, bought: [[row, units]],
//                      sold: [[row, units]] }
//                    { op: 'vendorHaggle', id, session, revision, round, offered, bought, sold }
//                    { op: 'vendorClosed', id, session, reason: 0 | 3 | 4 }

const { lookupItemClass } = require('../inventory/catalog.cjs');
const { itemPrice, metadataOf, propsOf } = require('../inventory/inventories.cjs');
const haggling = require('./haggle.cjs');
const { PlayerTrades } = require('./player_trades.cjs');

const MAX_ROWS = 128;
const MAX_BUY_ROWS = 128;
const MAX_AMOUNT = 1000000;
const MAX_PRICE = 1000000000;
const MAX_NAME_BYTES = 128;
const MAX_LINES = 256;

const CLOSE_PLAYER = 0;
const CLOSE_SERVER = 1;
const CLOSE_REPLACED = 2;
const CLOSE_FAILED = 3;
const CLOSE_LEFT = 4;
// What the player's game reports, as vendorClosed's reason.
const CLIENT_REASONS = new Map([
  [0, CLOSE_PLAYER],
  [3, CLOSE_FAILED],
  [4, CLOSE_LEFT],
]);

function isWholeNumber(value, low, high) {
  return Number.isInteger(value) && value >= low && value <= high;
}

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function refusal(code) {
  return { ok: false, code };
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

function moneyClass() {
  return lookupItemClass('money');
}

// One basket's key: the shelf's revision and its lines in row order. A haggle is over one key.
function basketKey(revision, bought, sold) {
  const sorted = (lines) => {
    return [...lines].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  };
  return JSON.stringify([revision, sorted(bought), sorted(sold)]);
}

class Vendors {
  constructor(host) {
    this.host = host;
    this.vendors = new Map(); // vendor id -> vendor
    this.sessions = new Map(); // session id -> session
    this.byPlayer = new Map(); // player id -> session id
    this.nextVendor = 1;
    this.nextSession = 1;
    // A player's inventory as a shop for another player (Trade).
    this.trades = new PlayerTrades(this);
  }

  owner() {
    return this.host.context.getStore()?.name ?? null;
  }

  playerOf(value) {
    const id = typeof value === 'number' ? value : value?.id;
    if (!Number.isInteger(id)) {
      return null;
    }
    const player = this.host.players.get(id);
    return player && (typeof value === 'number' || player === value) ? player : null;
  }

  create(options) {
    if (options !== undefined && !isObject(options)) {
      throw new TypeError('Vendor.create options must be an object');
    }
    const { name = '', purse, buys = true, haggle } = options ?? {};
    if (typeof name !== 'string' || Buffer.byteLength(name) > MAX_NAME_BYTES) {
      throw new TypeError(
        `Vendor.create options.name is a string of at most ${MAX_NAME_BYTES} bytes`,
      );
    }
    if (purse !== undefined && !isWholeNumber(purse, 0, MAX_PRICE)) {
      throw new RangeError(`Vendor.create options.purse is a whole number from 0 to ${MAX_PRICE}`);
    }
    if (typeof buys !== 'boolean') {
      throw new TypeError('Vendor.create options.buys must be a boolean');
    }
    const haggleOptions = haggling.haggleOptionsOf(haggle);
    const vendor = {
      id: this.nextVendor++,
      name,
      purse: purse ?? null,
      buys,
      // null: no haggling; else { tolerance, rounds }
      haggle: haggleOptions,
      // [{ id, itemClass, props, amount, price }]
      shelf: [],
      // item class id -> { itemClass, price }
      buyPrices: new Map(),
      revision: 1,
      nextRow: 1,
      owner: this.owner(),
    };
    this.vendors.set(vendor.id, vendor);
    return vendor.id;
  }

  destroy(vendorId) {
    const vendor = this.vendors.get(vendorId);
    if (!vendor) {
      return false;
    }
    for (const session of this.sessionsOf(vendor)) {
      this.end(session, CLOSE_SERVER, true);
    }
    this.vendors.delete(vendor.id);
    return true;
  }

  // An item's price on this server at the metadata's quality: the item config's or an override's
  // base price, else the game's own (Inventory.itemPrice's pristine price).
  serverPrice(itemClass, metadata) {
    const priced = itemPrice(itemClass.id, metadata, (priced) => {
      return this.host.itemOverrides.effectiveOf(priced).price;
    });
    return Math.min(priced ? priced.pristine : 0, MAX_PRICE);
  }

  // The shelf from a script's rows; throws for unusable ones.
  shelfOf(vendor, rows) {
    if (!Array.isArray(rows) || rows.length > MAX_ROWS) {
      throw new TypeError(`Vendor.setShelf takes an array of at most ${MAX_ROWS} rows`);
    }
    const seen = new Set();
    return rows.map((row) => {
      if (!isObject(row)) {
        throw new TypeError('Vendor.setShelf rows are { item, amount, price, metadata? }');
      }
      const itemClass = lookupItemClass(row.item);
      if (!itemClass || itemClass.quest) {
        throw new TypeError(
          `Vendor.setShelf: ${String(row.item)} is not an item class it can sell`,
        );
      }
      if (seen.has(itemClass.id)) {
        throw new TypeError(`Vendor.setShelf: ${itemClass.name} is listed twice`);
      }
      seen.add(itemClass.id);
      if (!isWholeNumber(row.amount, 1, MAX_AMOUNT)) {
        throw new RangeError(`Vendor.setShelf: amount is a whole number from 1 to ${MAX_AMOUNT}`);
      }
      if (row.price !== undefined && !isWholeNumber(row.price, 0, MAX_PRICE)) {
        throw new RangeError(`Vendor.setShelf: price is a whole number from 0 to ${MAX_PRICE}`);
      }
      const { props, code } = propsOf(itemClass, row.metadata);
      if (code) {
        throw new TypeError(`Vendor.setShelf: unusable metadata (${code})`);
      }
      return {
        id: `s${vendor.nextRow++}`,
        itemClass,
        props,
        amount: row.amount,
        price: row.price ?? this.serverPrice(itemClass, row.metadata),
      };
    });
  }

  setShelf(vendorId, rows) {
    const vendor = this.vendors.get(vendorId);
    if (!vendor) {
      return false;
    }
    vendor.shelf = this.shelfOf(vendor, rows);
    this.changed(vendor);
    return true;
  }

  setOffers(vendorId, rows) {
    const vendor = this.vendors.get(vendorId);
    if (!vendor) {
      return false;
    }
    if (!Array.isArray(rows) || rows.length > MAX_BUY_ROWS) {
      throw new TypeError(`Vendor.setOffers takes an array of at most ${MAX_BUY_ROWS} rows`);
    }
    const prices = new Map();
    for (const row of rows) {
      if (!isObject(row)) {
        throw new TypeError('Vendor.setOffers rows are { item, price }');
      }
      const itemClass = lookupItemClass(row.item);
      if (!itemClass) {
        throw new TypeError(`Vendor.setOffers: ${String(row.item)} is not an item class`);
      }
      if (prices.has(itemClass.id)) {
        throw new TypeError(`Vendor.setOffers: ${itemClass.name} is listed twice`);
      }
      if (row.price !== undefined && !isWholeNumber(row.price, 0, MAX_PRICE)) {
        throw new RangeError(`Vendor.setOffers: price is a whole number from 0 to ${MAX_PRICE}`);
      }
      const price = row.price ?? this.serverPrice(itemClass, undefined);
      prices.set(itemClass.id, { itemClass, price });
    }
    vendor.buyPrices = prices;
    this.changed(vendor);
    return true;
  }

  fillPurse(vendorId, purse) {
    const vendor = this.vendors.get(vendorId);
    if (!vendor) {
      return false;
    }
    if (purse !== null && !isWholeNumber(purse, 0, MAX_PRICE)) {
      throw new RangeError(`Vendor.fillPurse takes null or a whole number from 0 to ${MAX_PRICE}`);
    }
    vendor.purse = purse;
    for (const session of this.sessionsOf(vendor)) {
      this.show(session, false);
    }
    return true;
  }

  purseOf(vendorId) {
    return this.vendors.get(vendorId)?.purse ?? null;
  }

  open(vendorId, playerValue, npcValue) {
    const vendor = this.vendors.get(vendorId);
    const player = this.playerOf(playerValue);
    if (!vendor || !player || !player.ready) {
      return 0;
    }
    let npc = null;
    if (npcValue !== undefined && npcValue !== null) {
      const id = typeof npcValue === 'number' ? npcValue : npcValue?.id;
      npc = this.host.npcs.npcs.get(id) ?? null;
      if (!npc || !this.host.npcs.live(npc) || !npc.alive) {
        return 0;
      }
    }
    // The game has one trade screen: the session the player is in ends first, and one a
    // vendorClosed handler opened in its place too.
    for (let round = 0; round < 2; round += 1) {
      const previous = this.byPlayer.get(player.id);
      if (previous !== undefined) {
        this.end(this.sessions.get(previous), CLOSE_REPLACED, false);
      }
    }
    if (!player.ready || this.host.players.get(player.id) !== player) {
      return 0;
    }
    if (!this.vendors.has(vendor.id)) {
      return 0;
    }
    return this.openSession(vendor, player, npc);
  }

  // A session of the vendor (or of a player trade's) on the player's shop screen.
  openSession(vendor, player, npc) {
    for (let round = 0; round < 2; round += 1) {
      const previous = this.byPlayer.get(player.id);
      if (previous !== undefined) {
        this.end(this.sessions.get(previous), CLOSE_REPLACED, false);
      }
    }
    if (!player.ready || this.host.players.get(player.id) !== player) {
      return 0;
    }
    // haggle: the haggle over the basket the player haggles for (haggle.cjs startHaggle), or null.
    const session = {
      id: this.nextSession++,
      vendor,
      player,
      npc,
      owner: this.owner(),
      haggle: null,
      refused: false,
    };
    if (this.nextSession > 0xffffffff) {
      this.nextSession = 1;
    }
    this.sessions.set(session.id, session);
    this.byPlayer.set(player.id, session.id);
    this.show(session, true);
    return session.id;
  }

  close(sessionId) {
    const session = this.sessions.get(sessionId);
    if (!session) {
      return false;
    }
    this.end(session, CLOSE_SERVER, true);
    return true;
  }

  activeSession(playerValue) {
    const player = this.playerOf(playerValue);
    return (player && this.byPlayer.get(player.id)) || 0;
  }

  shopOf(sessionId) {
    return this.sessions.get(sessionId)?.vendor.id ?? 0;
  }

  sessionsOf(vendor) {
    return [...this.sessions.values()].filter((session) => session.vendor === vendor);
  }

  // A shelf or a buy list changed: baskets priced against the old one are refused, and every
  // screen open on the vendor shows the new one.
  changed(vendor) {
    vendor.revision += 1;
    for (const session of this.sessionsOf(vendor)) {
      this.show(session, false);
    }
  }

  show(session, open) {
    const { vendor, player } = session;
    if (!player.ready || this.host.players.get(player.id) !== player) {
      return;
    }
    const shownClassOf = (row) => this.host.customItems?.shownClassOf(row) ?? row.itemClass.id;
    this.host.bridge.send({
      op: 'vendorShow',
      id: player.id,
      session: session.id,
      open,
      buys: vendor.buys,
      // A player's trade: the seller keeps the shop.
      npc: vendor.seller ? vendor.seller.id : session.npc ? session.npc.id : 0,
      keeperPlayer: Boolean(vendor.seller),
      // The game's haggle needs a trader to talk to: an NPC keeper.
      haggles: Boolean(vendor.haggle) && !vendor.seller && Boolean(session.npc),
      revision: vendor.revision,
      purse: vendor.purse,
      money: moneyClass().id,
      rows: vendor.shelf.map((row) => [
        row.id,
        shownClassOf(row),
        row.amount,
        row.props.quality,
        row.props.health,
        0,
      ]),
      prices: vendor.shelf.map((row) => row.price),
      buyPrices: vendor.buys
        ? [...vendor.buyPrices.values()].map(({ itemClass, price }) => [itemClass.id, price])
        : [],
    });
  }

  end(session, reason, tellClient) {
    if (!session || this.sessions.get(session.id) !== session) {
      return;
    }
    this.sessions.delete(session.id);
    const player = session.player;
    if (this.byPlayer.get(player.id) === session.id) {
      this.byPlayer.delete(player.id);
    }
    if (tellClient && player.ready && this.host.players.get(player.id) === player) {
      this.host.bridge.send({ op: 'vendorClose', id: player.id, session: session.id });
    }
    if (session.vendor.seller) {
      this.host.emit('tradeClosed', session.vendor.seller, player, reason);
      return;
    }
    this.host.emit('vendorClosed', session.vendor.id, player, reason);
  }

  // What a confirmed basket would move, for Inventory.beforeCommit: kind 'vendor' with the shop's
  // `vendor` id (another player's shelf: player_trades.cjs), lines bought (to the player) and sold
  // (from the player) with their prices, and `money` the player gets (negative: pays), at a price
  // agreed in the haggle when there is one. Null when the basket cannot settle as it is.
  describeTrade(player, sessionId, revision, boughtList, soldList) {
    const session = this.sessions.get(sessionId);
    if (!player || !session || session.player !== player) {
      return null;
    }
    const vendor = session.vendor;
    if (vendor.seller) {
      return this.trades.describe(player, session, revision, boughtList, soldList);
    }
    const priced = this.priceBasket(player, vendor, revision, boughtList, soldList);
    if (!priced.ok) {
      return null;
    }
    const haggle = session.haggle;
    const haggled =
      haggle !== null && haggle.agreed !== null && haggle.key === priced.key ? haggle.agreed : null;
    const lines = [];
    for (const [row, units] of priced.fromShelf) {
      lines.push({
        item: row.itemClass.id,
        name: row.itemClass.name,
        amount: units,
        metadata: metadataOf(row.itemClass, row.props),
        price: row.price,
        from: 'vendor',
        to: 'player',
      });
    }
    for (const line of priced.soldLines) {
      lines.push({ ...line, from: 'player', to: 'vendor' });
    }
    return {
      kind: 'vendor',
      vendor: vendor.id,
      money: haggled === null ? priced.pay - priced.cost : -haggled,
      lines,
    };
  }

  // The basket the player confirmed on the screen of `sessionId`. Returns { ok, code } (the code
  // names why a basket was refused; nothing moved then).
  trade(player, sessionId, revision, boughtList, soldList) {
    const session = this.sessions.get(sessionId);
    if (!player || !session || session.player !== player) {
      return refusal('noSession');
    }
    if (session.vendor.seller) {
      return this.trades.settle(player, session, revision, boughtList, soldList);
    }
    const vendor = session.vendor;
    const priced = this.priceBasket(player, vendor, revision, boughtList, soldList);
    if (!priced.ok) {
      return priced;
    }
    const { inventory, money, fromShelf, take, soldLines, cost, pay } = priced;
    // The basket the vendor agreed a price for in the haggle settles at that price.
    const haggle = session.haggle;
    const haggled =
      haggle !== null && haggle.agreed !== null && haggle.key === priced.key ? haggle.agreed : null;
    const balance = haggled === null ? pay - cost : -haggled;
    if (balance > 0 && vendor.purse !== null && vendor.purse < balance) {
      return refusal('vendorCannotPay');
    }
    if (Math.abs(balance) > MAX_AMOUNT) {
      return refusal('inventoryCapacity');
    }
    const gains = [...fromShelf].map(([row, units]) => ({
      itemClass: row.itemClass,
      props: row.props,
      amount: units,
    }));
    const { result, change } = inventory.settle(take, money, balance, gains);
    if (!result.ok) {
      return refusal(result.code);
    }
    session.haggle = null;
    this.host.playerInventories.record(player, change);
    this.host.playerInventories.announce(player, change);
    const boughtLines = [];
    for (const [row, units] of fromShelf) {
      row.amount -= units;
      boughtLines.push({
        metadata: metadataOf(row.itemClass, row.props),
        item: row.itemClass.id,
        name: row.itemClass.name,
        amount: units,
        price: row.price,
      });
    }
    vendor.shelf = vendor.shelf.filter((row) => row.amount > 0);
    if (vendor.purse !== null) {
      vendor.purse = Math.min(MAX_PRICE, vendor.purse - balance);
    }
    this.changed(vendor);
    this.host.emit(
      'vendorTrade',
      vendor.id,
      player,
      boughtLines,
      soldLines,
      balance,
      haggled !== null,
    );
    return { ok: true, code: '' };
  }

  // A basket on the vendor's shelf of `revision`, priced at the vendor's lists: { ok: true, key,
  // inventory, money, fromShelf (shelf row -> units), take (player's row -> units), soldLines,
  // cost (what the player pays for the shelf rows), pay (what the vendor pays for theirs) }, or a
  // refusal.
  priceBasket(player, vendor, revision, boughtList, soldList) {
    const bought = linesOf(boughtList);
    const sold = linesOf(soldList);
    if (!bought || !sold || bought.size + sold.size === 0) {
      return refusal('invalidRequest');
    }
    if (revision !== vendor.revision) {
      return refusal('staleRevision');
    }
    const entry = this.host.playerInventories.entryOf(player);
    if (!entry) {
      return refusal('inventoryUnavailable');
    }
    const inventory = entry.inventory;
    const money = moneyClass();
    // What the player buys: shelf row -> units.
    const fromShelf = new Map();
    let cost = 0;
    for (const [id, units] of bought) {
      const row = vendor.shelf.find((candidate) => candidate.id === id);
      if (!row) {
        return refusal('unknownItem');
      }
      if (units > row.amount) {
        return refusal('insufficientStock');
      }
      fromShelf.set(row, units);
      cost += units * row.price;
    }
    // What the player sells: their row -> units, priced from the buy list.
    if (sold.size > 0 && !vendor.buys) {
      return refusal('notBuying');
    }
    const take = new Map();
    const soldLines = [];
    let pay = 0;
    for (const [id, units] of sold) {
      const row = inventory.rows.get(id);
      if (!row) {
        return refusal('unknownItem');
      }
      const listed = vendor.buyPrices.get(row.itemClass.id);
      if (!listed || row.itemClass === money || row.itemClass.quest) {
        return refusal('notBought');
      }
      if (units > row.amount) {
        return refusal('insufficientItems');
      }
      take.set(row, units);
      pay += units * listed.price;
      soldLines.push({
        metadata: metadataOf(row.itemClass, row.props),
        item: row.itemClass.id,
        name: row.itemClass.name,
        amount: units,
        price: listed.price,
      });
    }
    return {
      ok: true,
      code: '',
      key: basketKey(revision, [...bought], [...sold]),
      inventory,
      money,
      fromShelf,
      take,
      soldLines,
      cost,
      pay,
    };
  }

  // A price the player offered in the haggle on the screen of `sessionId` (0.4.1): answered with
  // vendorHaggleReply. Returns the answer { answer, price, mood } that went, or a refusal (nothing
  // went then: the client's offer did not fit the session).
  haggle(player, sessionId, revision, round, offered, boughtList, soldList) {
    const session = this.sessions.get(sessionId);
    if (!player || !session || session.player !== player) {
      return refusal('noSession');
    }
    const vendor = session.vendor;
    if (!Number.isInteger(round) || round < 1 || !haggling.wholePrice(offered)) {
      return refusal('invalidRequest');
    }
    const reply = (result) => {
      this.host.bridge.send({
        op: 'vendorHaggleReply',
        id: player.id,
        session: session.id,
        round,
        answer: result.answer,
        price: result.price,
        mood: result.mood,
      });
      return result;
    };
    if (!vendor.haggle || vendor.seller || session.refused) {
      return reply({ answer: haggling.REFUSE, price: offered, mood: 100 });
    }
    const priced = this.priceBasket(player, vendor, revision, boughtList, soldList);
    if (!priced.ok) {
      return reply({ answer: haggling.REFUSE, price: offered, mood: 100 });
    }
    if (session.haggle === null || session.haggle.key !== priced.key) {
      session.haggle = haggling.startHaggle(
        priced.key,
        priced.cost - priced.pay,
        priced.cost + priced.pay,
      );
    }
    const haggle = session.haggle;
    haggle.round += 1;
    const basket = {
      bought: [...priced.fromShelf].map(([row, units]) => ({
        metadata: metadataOf(row.itemClass, row.props),
        item: row.itemClass.id,
        name: row.itemClass.name,
        amount: units,
        price: row.price,
      })),
      sold: priced.soldLines,
      balance: priced.pay - priced.cost,
      price: haggle.price,
      asked: haggle.asked,
      round: haggle.round,
      mood: Math.round(haggle.impatience * 100) / 100,
    };
    let result = null;
    const answered = this.host.emitCollecting(
      'vendorHaggle',
      (value, resource) => {
        if (result !== null) {
          return;
        }
        try {
          result = haggling.scriptAnswer(value, haggle, offered);
        } catch (error) {
          this.host.bridge.log(resource, `vendorHaggle answer refused: ${error.message}`);
        }
      },
      vendor.id,
      player,
      basket,
      offered,
    );
    if (result === null && !answered) {
      result = {
        answer: haggling.REFUSE,
        price: haggle.asked,
        mood: Math.round(haggle.impatience * 100),
      };
    }
    if (result === null) {
      const speech = player.levels?.speech;
      result = haggling.ruleAnswer(
        haggle,
        offered,
        vendor.haggle,
        haggling.dominanceOf(speech),
        vendor.purse,
      );
    }
    haggle.lastOffer = offered;
    haggle.impatience = result.mood / 100;
    if (result.answer === haggling.ACCEPT) {
      haggle.agreed = result.price;
    } else if (result.answer === haggling.REFUSE) {
      session.refused = true;
      session.haggle = null;
    } else {
      haggle.asked = result.price;
    }
    return reply(result);
  }

  // The screen went away on the player's side.
  closed(player, sessionId, reason) {
    const session = this.sessions.get(sessionId);
    if (!player || !session || session.player !== player) {
      return false;
    }
    this.end(session, CLIENT_REASONS.get(reason) ?? CLOSE_FAILED, false);
    return true;
  }

  sessionOfPlayer(player) {
    const id = this.byPlayer.get(player.id);
    return id === undefined ? null : this.sessions.get(id);
  }

  // The player disconnects, or loads a world again: their game shows nothing of the old one.
  left(player) {
    this.end(this.sessionOfPlayer(player), CLOSE_LEFT, false);
    this.trades.sellerGone(player, CLOSE_LEFT);
  }

  arrived(player) {
    this.end(this.sessionOfPlayer(player), CLOSE_LEFT, false);
  }

  knockedOut(player) {
    this.end(this.sessionOfPlayer(player), CLOSE_SERVER, true);
    this.trades.sellerGone(player, CLOSE_SERVER);
  }

  // The NPC keeping a shop died or is being removed.
  npcGone(npc) {
    for (const session of [...this.sessions.values()]) {
      if (session.npc === npc) {
        this.end(session, CLOSE_SERVER, true);
      }
    }
  }

  // The vendors a resource created go with it, and the sessions it opened end.
  resourceStopped(name) {
    for (const session of [...this.sessions.values()]) {
      if (session.owner === name) {
        this.end(session, CLOSE_SERVER, true);
      }
    }
    for (const vendor of [...this.vendors.values()]) {
      if (vendor.owner === name) {
        this.destroy(vendor.id);
      }
    }
  }

  api() {
    const self = this;
    return Object.freeze({
      create: (options) => self.create(options),
      destroy: (vendor) => self.destroy(vendor),
      setShelf: (vendor, rows) => self.setShelf(vendor, rows),
      setOffers: (vendor, rows) => self.setOffers(vendor, rows),
      fillPurse: (vendor, purse) => self.fillPurse(vendor, purse),
      purseOf: (vendor) => self.purseOf(vendor),
      open: (vendor, player, npc) => self.open(vendor, player, npc),
      close: (session) => self.close(session),
      activeSession: (player) => self.activeSession(player),
      shopOf: (session) => self.shopOf(session),
    });
  }
}

module.exports = {
  Vendors,
  CLOSE_PLAYER,
  CLOSE_SERVER,
  CLOSE_REPLACED,
  CLOSE_FAILED,
  CLOSE_LEFT,
};
