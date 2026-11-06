'use strict';

// Carrying another player's body from a resource script: who may carry whom, and the
// events a carry raises. The scenario runs with its script in JavaScript, in Lua and
// in C#.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  LANGUAGES,
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const RESCUE = {
  javascript: `
    kcds.core.Events.on('playerCarrying', (carrier, carried) => carrier.dimension === carried.dimension);
    kcds.core.Events.on('playerCarry', (carrier, carried) =>
      console.log('carry ' + carrier.nickname + ' ' + carried.nickname + ' ' + carrier.carrying.nickname));
    kcds.core.Events.on('playerPutDown', (carrier, carried) =>
      console.log(carrier.nickname + ' put ' + carried.nickname + ' down'));
    `,
  lua: `
    kcds.core.Events.on("playerCarrying", function(carrier, carried)
      return carrier.dimension == carried.dimension
    end)
    kcds.core.Events.on("playerCarry", function(carrier, carried)
      print("carry " .. carrier.nickname .. " " .. carried.nickname .. " " .. carrier.carrying.nickname)
    end)
    kcds.core.Events.on("playerPutDown", function(carrier, carried)
      print(carrier.nickname .. " put " .. carried.nickname .. " down")
    end)
    `,
  csharp: `
public class Rescue : Script
{
    public override void OnStart()
    {
        Events.On("playerCarrying", (Player carrier, Player carried) => carrier.Dimension == carried.Dimension);
        Events.On("playerCarry", (Player carrier, Player carried) =>
        {
            Log.Info("carry " + carrier.Nickname + " " + carried.Nickname + " " + carrier.Carrying?.Nickname);
        });
        Events.On("playerPutDown", (Player carrier, Player carried) =>
        {
            Log.Info(carrier.Nickname + " put " + carried.Nickname + " down");
        });
    }
}
`,
};

for (const language of LANGUAGES) {
  test(`carrying (${language}): a gamemode decides who carries whom and hears the carry's course`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'rescue', RESCUE, {}, language);
    await f.host.startAll();
    const carrier = f.report(3);
    const elsewhere = f.report(4, { dimension: 2 });
    const carried = f.report(5);
    f.host.playerDied(5);
    // A carrier in another dimension is refused: their game lets go at once.
    f.host.bodyCarries.report(elsewhere, { kind: 'started', carried: 5 });
    assert.equal(carried.heldBy, null);
    assert.deepEqual(
      f.sent.filter((e) => e.op === 'bodyCarry').map((e) => [e.id, e.kind]),
      [[4, 'refuse']],
    );
    f.host.bodyCarries.report(carrier, { kind: 'started', carried: 5 });
    assert.equal(carrier.carrying, carried);
    assert.equal(carried.heldBy, carrier);
    // putDown has them put the body down; false once they carry nobody.
    assert.equal(carrier.putDown(), true);
    f.host.bodyCarries.report(carrier, { kind: 'ended', carried: 5 });
    assert.equal(carrier.carrying, null);
    assert.equal(carrier.putDown(), false);
    assert.deepEqual(
      f.linesOf('rescue').filter((line) => line !== 'started'),
      ['carry Player 3 Player 5 Player 5', 'Player 3 put Player 5 down'],
    );
  });
}
