'use strict';

// The crafting-kit resource (Backend/resources/crafting-kit): a player who joins learns every
// alchemy and smithing recipe and gets their materials; /kit gives them again.

const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const SERVER = fs.readFileSync(
  path.join(__dirname, '../../../resources/crafting-kit/server.js'),
  'utf8',
);

function amountOf(player, name) {
  let amount = 0;
  for (const row of player.getInventory().items) {
    if (row.name === name) {
      amount += row.amount;
    }
  }
  return amount;
}

async function joined(t) {
  const fixture = serverFixture(t);
  serverResource(fixture.root, 'crafting-kit', SERVER);
  await fixture.host.startAll();
  const player = fixture.report(7, { gameplayReady: true });
  return { ...fixture, player };
}

test('a player who joins learns every recipe and gets the materials', async (t) => {
  const { host, sent, player, report } = await joined(t);
  const { Recipe } = host.api;
  const recipes = Recipe.all().length;
  const perkOrders = sent.filter((message) => message.op === 'progressionOrder');
  assert.ok(perkOrders.length >= recipes, `${perkOrders.length} perk orders for ${recipes}`);
  // Alchemy herbs and bases, smithing materials; no quest piece.
  assert.ok(amountOf(player, 'herb_salvia') >= 6);
  assert.equal(amountOf(player, 'alchemySpiritus'), 0, 'the table has its own bases');
  assert.ok(amountOf(player, 'bsmt_ironGood') >= 9);
  assert.equal(amountOf(player, 'kovaniAsiDoVezi_holyNail'), 0);
  assert.equal(amountOf(player, 'maliruvLek_mandrakeRoot'), 0);
  assert.equal(amountOf(player, 'longswordHenry'), 0);
  assert.equal(amountOf(player, 'pomatenec_longsword_broken'), 0);
  // A rejoin starts with an empty inventory and gets the kit again.
  const salvia = amountOf(player, 'herb_salvia');
  host.disconnect(7);
  const again = report(7, { gameplayReady: true });
  assert.equal(amountOf(again, 'herb_salvia'), salvia);
  assert.ok(salvia > 0);
});

test('/kit gives the materials again, of one kind when named', async (t) => {
  const { host, player } = await joined(t);
  const salvia = amountOf(player, 'herb_salvia');
  const iron = amountOf(player, 'bsmt_ironGood');
  host.receiveChat(7, '/kit alchemy');
  assert.equal(amountOf(player, 'herb_salvia'), salvia * 2);
  assert.equal(amountOf(player, 'bsmt_ironGood'), iron);
  host.receiveChat(7, '/kit');
  assert.equal(amountOf(player, 'bsmt_ironGood'), iron * 2);
});
