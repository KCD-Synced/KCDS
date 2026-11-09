'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { encode, decode } = require('./codec.cjs');
function fixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-resource-'));
  const output = [],
    logs = [];
  const host = new ResourceHost(root, {
    // The time, weather and stew pots every arriving player is told are not
    // what these test.
    send: (e) => {
      if (!['worldTime', 'worldWeather', 'potLooks'].includes(e.op)) {
        output.push(e);
      }
    },
    log: (...e) => logs.push(e),
  });
  // These are resource lifecycle tests: every join here passes the licence gate without a store
  // identity (the gate itself: licence_gate.test.cjs, licence_admission.test.cjs).
  host.licence.check = async () => ({ allowed: true, reason: '', pass: null });
  // The needs orders every arriving player gets while the needs are off (their default, issue)
  // are not what these test.
  host.api.Needs.setEnabled(true);
  return { root, output, logs, host };
}
function resource(root, name, code, extra = {}) {
  const dir = path.join(root, name);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name, kcds: { serverScripts: ['index.js'], ...extra } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
}
function player(host, id = 7) {
  return host.updatePlayer({
    id,
    nickname: 'Henry',
    ready: true,
    position: { x: 10, y: 20, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}
test('existing Value wire format round trip and rejects trailing/truncated', () => {
  const source = {
    op: 'horseSpawn',
    id: 1879048192,
    position: { x: 1.25, y: 3, z: -7 },
    ready: true,
    args: ['horse', null],
  };
  assert.deepEqual(JSON.parse(JSON.stringify(decode(encode(source)))), source);
  assert.throws(() => decode(Buffer.concat([encode(source), Buffer.from([0])])));
  assert.throws(() => decode(encode(source).subarray(0, 5)));
  assert.equal(decode(Buffer.from([2, 13])), -7);
});
test('resource owns horse command, CCDC whitespace/case contract, cleanup on stop', async () => {
  const f = fixture();
  resource(
    f.root,
    'horse',
    `kcds.core.Events.on('playerCommand',(p,c,args)=>{if(c.toLowerCase()!=='horse')return;const pos=p.position.clone().add(p.rotation.rotateVector(new kcds.math.Vector3(0,3,0)));const h=kcds.characters.Horse.spawn(pos,p.rotation,undefined,args.join('|'));h.giveTo(p);kcds.core.Chat.sendToPlayer(p,'created');});`,
  );
  try {
    await f.host.startAll();
    player(f.host);
    f.host.receiveChat(7, '/Horse  "Pebbles two"');
    const h = f.output.find((e) => e.op === 'horseSpawn');
    assert.equal(h.position.y, 23);
    assert.equal(h.name, '"Pebbles|two"');
    assert.equal(f.output.filter((e) => e.op === 'chat').length, 1);
    await f.host.stop('horse');
    assert.equal(f.host.api.Horse.all().length, 0);
    assert.equal(f.output.at(-1).op, 'horseDestroy');
    f.host.receiveChat(7, '/horse');
    assert.equal(f.output.filter((e) => e.op === 'horseSpawn').length, 1);
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});
test('default relay precedes playerChat, relay off and command never broadcasts', () => {
  const f = fixture();
  const p = player(f.host);
  f.host.api.Events.on('playerChat', () => assert.equal(f.output.at(-1).author, 'Henry'));
  f.host.receiveChat(p.id, 'hello');
  assert.equal(f.output[0].text, 'hello');
  f.host.api.Chat.setChatRelay(false);
  f.host.receiveChat(p.id, 'quiet');
  f.host.receiveChat(p.id, '/NoNativeCommand');
  assert.equal(f.output.length, 1);
  assert.equal(f.host.receiveChat(999, 'forged'), false);
  assert.equal(f.host.receiveChat(p.id, 'bad\0text'), false);
  fs.rmSync(f.root, { recursive: true, force: true });
});
test('required dependency failure and cycles start no scripts', async () => {
  const f = fixture();
  resource(f.root, 'a', `console.log('should not run')`, { requires: ['b'] });
  try {
    await assert.rejects(f.host.startAll(), /Resource 'a' depends on missing resource 'b'/);
    assert.equal(f.host.resources.size, 0);
    resource(f.root, 'b', '', { requires: ['a'] });
    await assert.rejects(f.host.startAll(), /cycle/);
    assert.equal(f.host.resources.size, 0);
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});
test('CommonJS imports run with same globals and ensure does not duplicate handlers', async () => {
  const f = fixture();
  resource(f.root, 'horse', `require('./commands.js');`);
  fs.writeFileSync(
    path.join(f.root, 'horse', 'commands.js'),
    `kcds.core.Events.on('playerCommand',(p,c)=>{if(c==='horse')kcds.core.Chat.sendToPlayer(p,'one');});`,
  );
  try {
    await f.host.startAll();
    player(f.host);
    f.host.receiveChat(7, '/horse');
    await f.host.ensure('horse');
    f.host.receiveChat(7, '/horse');
    assert.equal(f.output.length, 2);
  } finally {
    await f.host.stop('horse');
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});

test('server resource state bindings retain entity data across reload and clean listeners and stale handles', async (t) => {
  const f = fixture();
  t.after(async () => {
    await f.host.shutdown();
    fs.rmSync(f.root, { recursive: true, force: true });
  });
  resource(
    f.root,
    'state-mode',
    `
    const player = kcds.players.Player.all()[0];
    if (!(player.state instanceof kcds.core.SharedState)) throw Error('SharedState binding missing');
    if (!player.state.has('score')) player.state.set('score', 1);
    player.state.onChange('score', (key, value) => kcds.core.Chat.sendToAll('score=' + value));
    kcds.core.Events.on('entityStateChange', (entity, key, value) => {
      if(entity.id === player.id && key === 'score') kcds.core.Chat.sendToAll('event=' + value);
    });
  `,
  );
  const p = player(f.host);
  const bag = p.state;
  await f.host.startAll();
  bag.set('score', 2);
  assert.deepEqual(
    f.output.filter((value) => value.op === 'chat').map((value) => value.text),
    ['score=2', 'event=2'],
  );
  await f.host.stop('state-mode');
  bag.set('score', 3);
  assert.equal(f.output.filter((value) => value.op === 'chat').length, 2);
  await f.host.start('state-mode');
  assert.equal(p.state.get('score'), 3);
  bag.set('score', 4);
  assert.deepEqual(
    f.output
      .filter((value) => value.op === 'chat')
      .slice(-2)
      .map((value) => value.text),
    ['score=4', 'event=4'],
  );
  assert.throws(() => {
    p.id = 8;
  }, TypeError);
  f.host.disconnect(p.id);
  const replacement = player(f.host, p.id);
  assert.equal(replacement.state.get('score'), undefined);
  assert.throws(() => bag.get('score'), /no longer exists/);
  assert.throws(() => p.state, /no longer exists/);
});
test('sequential player ids: a stale id never resolves after reconnect, playerIndex is the slot', () => {
  const f = fixture();
  const join = (id, playerIndex) =>
    f.host.updatePlayer({
      id,
      playerIndex,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  const { Player } = f.host.api;
  const first = join(1, 0);
  const second = join(2, 1);
  assert.equal(first.id, 1);
  assert.equal(first.nickname, 'Player 1');
  assert.equal(first.playerIndex, 0);
  assert.equal(second.nickname, 'Player 2');
  assert.equal(Player.getById(1), first);

  // The first player reconnects: the server hands out the next id and may reuse the slot.
  f.host.disconnect(1);
  const reconnected = join(3, 0);
  assert.equal(Player.getById(1), null);
  assert.throws(() => new Player(1), /Unknown player/);
  assert.equal(Player.getById(3), reconnected);
  assert.notEqual(reconnected, first);
  assert.equal(reconnected.nickname, 'Player 3');
  assert.equal(reconnected.playerIndex, 0);
  assert.deepEqual(
    Player.all()
      .map((player) => player.id)
      .sort((a, b) => a - b),
    [2, 3],
  );

  // A player report without a slot reads as unassigned, never as the network id.
  const unassigned = join(4, undefined);
  assert.equal(unassigned.playerIndex, 65535);
});
test('Entity.position assignment forces the player position through the native bridge', async () => {
  const f = fixture();
  resource(
    f.root,
    'mover',
    `kcds.core.Events.on('playerCommand', (p, c) => {
      if (c === 'move') {
        p.position = new kcds.math.Vector3(100, 200, 30);
      }
      if (c === 'bad') {
        try {
          p.position = { x: NaN, y: 0, z: 0 };
        } catch (error) {
          kcds.core.Chat.sendToPlayer(p, error.constructor.name);
        }
      }
    });`,
  );
  try {
    await f.host.startAll();
    const p = player(f.host);
    assert.equal(p.position.x, 10);

    f.host.receiveChat(p.id, '/move');
    const forced = f.output.filter((e) => e.op === 'entityPosition');
    assert.equal(forced.length, 1);
    assert.deepEqual(forced[0], {
      op: 'entityPosition',
      id: 7,
      position: { x: 100, y: 200, z: 30 },
    });
    assert.equal(p.position.x, 100);
    assert.ok(p.position instanceof f.host.api.Vector3);

    f.host.receiveChat(p.id, '/bad');
    assert.equal(f.output.filter((e) => e.op === 'entityPosition').length, 1);
    assert.equal(f.output.at(-1).text, 'TypeError');
    assert.equal(p.position.x, 100);

    // The next native report remains authoritative for reads.
    f.host.updatePlayer({ id: 7, ready: true, position: { x: 1, y: 2, z: 3 }, rotation: { w: 1 } });
    assert.equal(p.position.x, 1);

    await f.host.stop('mover');
    const stale = f.host.players.get(7);
    assert.throws(() => {
      f.host.context.run({ name: 'mover', active: false }, () => {
        stale.position = { x: 0, y: 0, z: 0 };
      });
    }, /stopped/);
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});

test('Entity wraps existing players and horses, and Player handles inherit from it', async () => {
  const f = fixture();
  resource(
    f.root,
    'entities',
    `kcds.core.Events.on('playerCommand', (p, c) => {
      if (c !== 'check') {
        return;
      }
      const same = new kcds.players.Entity(p.id);
      const horse = kcds.characters.Horse.spawn({ x: 1, y: 2, z: 3 });
      const results = [
        same === p,
        p instanceof kcds.players.Entity,
        p instanceof kcds.players.Player,
        horse instanceof kcds.players.Entity,
        new kcds.players.Entity(horse.id) === horse,
        String(p),
        String(horse),
        kcds.players.Entity.prototype.toString.call({ id: 9 }),
      ];
      for (const bad of [0, -1, 1.5, '7', 0x100000000]) {
        try {
          new kcds.players.Entity(bad);
          results.push('accepted');
        } catch (error) {
          results.push(error.constructor.name);
        }
      }
      try {
        new kcds.players.Entity(424242);
        results.push('accepted');
      } catch (error) {
        results.push(error.message);
      }
      kcds.core.Chat.sendToPlayer(p, JSON.stringify(results));
    });`,
  );
  try {
    await f.host.startAll();
    const p = player(f.host);
    f.host.receiveChat(p.id, '/check');
    assert.deepEqual(JSON.parse(f.output.at(-1).text), [
      true,
      true,
      true,
      true,
      true,
      'Player(7, "Henry")',
      'Horse(1879048192)',
      'Entity(9)',
      'TypeError',
      'TypeError',
      'TypeError',
      'TypeError',
      'TypeError',
      'Unknown entity',
    ]);
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});

test('Entity.rotation assignment forces a unit quaternion through the native bridge', async () => {
  const f = fixture();
  resource(
    f.root,
    'turner',
    `kcds.core.Events.on('playerCommand', (p, c) => {
      if (c === 'quat') {
        p.rotation = new kcds.math.Quaternion(2, 0, 0, 0);
      }
      if (c === 'euler') {
        p.rotation = { x: 0, y: 0, z: 90 };
      }
      if (c === 'partial') {
        p.rotation = { z: 90 };
      }
      if (c === 'bad') {
        for (const value of [null, { x: NaN, y: 0, z: 0 }, { w: 0, x: 0, y: 0, z: 0 }]) {
          try {
            p.rotation = value;
          } catch (error) {
            kcds.core.Chat.sendToPlayer(p, error.constructor.name);
          }
        }
      }
    });`,
  );
  try {
    await f.host.startAll();
    const p = player(f.host);
    assert.ok(p.rotation instanceof f.host.api.Quaternion);

    f.host.receiveChat(p.id, '/quat');
    let forced = f.output.filter((e) => e.op === 'entityRotation');
    assert.equal(forced.length, 1);
    assert.deepEqual(forced[0], {
      op: 'entityRotation',
      id: 7,
      rotation: { x: 0, y: 0, z: 0, w: 1 },
    });
    assert.equal(p.rotation.w, 1);

    f.host.receiveChat(p.id, '/euler');
    forced = f.output.filter((e) => e.op === 'entityRotation');
    assert.equal(forced.length, 2);
    const q = forced[1].rotation;
    assert.ok(Math.abs(Math.hypot(q.x, q.y, q.z, q.w) - 1) < 1e-9);
    assert.ok(Math.abs(Math.abs(q.z) - Math.SQRT1_2) < 1e-9);
    assert.ok(Math.abs(Math.abs(q.w) - Math.SQRT1_2) < 1e-9);

    f.host.receiveChat(p.id, '/bad');
    assert.equal(f.output.filter((e) => e.op === 'entityRotation').length, 2);
    assert.deepEqual(
      f.output.slice(-3).map((e) => e.text),
      ['TypeError', 'TypeError', 'TypeError'],
    );

    // An axis left out of Euler angles is 0, as the docs' example has it.
    f.host.receiveChat(p.id, '/partial');
    forced = f.output.filter((e) => e.op === 'entityRotation');
    assert.equal(forced.length, 3);
    assert.deepEqual(forced[2].rotation, forced[1].rotation);

    // The next native report remains authoritative for reads.
    f.host.updatePlayer({ id: 7, ready: true, position: { x: 1, y: 2, z: 3 }, rotation: { w: 1 } });
    assert.equal(p.rotation.w, 1);
    assert.equal(p.rotation.z, 0);

    await f.host.stop('turner');
    const stale = f.host.players.get(7);
    assert.throws(() => {
      f.host.context.run({ name: 'turner', active: false }, () => {
        stale.rotation = { w: 1, x: 0, y: 0, z: 0 };
      });
    }, /stopped/);
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});

test('Player.setDimension validates the uint32 domain and settles reads on the native result', async () => {
  const f = fixture();
  resource(
    f.root,
    'worlds',
    `'use strict';
    kcds.core.Events.on('playerCommand', (p, c, args) => {
      if (c === 'world') {
        p.setDimension(Number(args[0]));
      }
      if (c === 'invalid') {
        const outcomes = [];
        for (const world of [-1, 1.5, 2 ** 32, NaN, Infinity, '1', undefined, null]) {
          try {
            p.setDimension(world);
            outcomes.push('accepted');
          } catch (error) {
            outcomes.push(error.constructor.name);
          }
        }
        kcds.core.Chat.sendToPlayer(p, outcomes.join(','));
      }
      if (c === 'assign') {
        try {
          p.dimension = 5;
        } catch (error) {
          kcds.core.Chat.sendToPlayer(p, error.constructor.name);
        }
      }
    });`,
  );
  try {
    await f.host.startAll();
    const p = player(f.host);
    assert.equal(p.dimension, 0xffffffff);
    f.host.updatePlayer({ id: 7, ready: true, dimension: 3, position: {}, rotation: {} });
    assert.equal(p.dimension, 3);

    f.host.receiveChat(p.id, '/world 1');
    const first = f.output.filter((e) => e.op === 'entityWorld');
    assert.equal(first.length, 1);
    assert.deepEqual({ ...first[0] }, { op: 'entityWorld', id: 7, dimension: 1, request: 1 });
    assert.equal(p.dimension, 1);

    // A native report produced before the request was applied must not revert the read.
    f.host.updatePlayer({ id: 7, ready: true, dimension: 3, position: {}, rotation: {} });
    assert.equal(p.dimension, 1);
    // Results for an older request do not settle the newest one.
    f.host.receiveChat(p.id, '/world 4294967295');
    f.host.confirmPlayerWorld({ id: 7, request: 1, dimension: 1 });
    assert.equal(p.dimension, 0xffffffff);
    f.host.confirmPlayerWorld({ id: 7, request: 2, dimension: 0xffffffff });
    assert.equal(p.dimension, 0xffffffff);
    // After settlement, native reports are authoritative again.
    f.host.updatePlayer({ id: 7, ready: true, dimension: 9, position: {}, rotation: {} });
    assert.equal(p.dimension, 9);

    // A rejected request settles on the authoritative dimension the native host returns.
    f.host.receiveChat(p.id, '/world 0');
    assert.equal(p.dimension, 0);
    f.host.confirmPlayerWorld({ id: 7, request: 3, dimension: 9 });
    assert.equal(p.dimension, 9);

    f.host.receiveChat(p.id, '/invalid');
    assert.equal(
      f.output.at(-1).text,
      'TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError',
    );
    assert.equal(f.output.filter((e) => e.op === 'entityWorld').length, 3);

    f.host.receiveChat(p.id, '/assign');
    assert.equal(f.output.at(-1).text, 'TypeError');
    assert.equal(p.dimension, 9);

    await f.host.stop('worlds');
    assert.throws(() => {
      f.host.context.run({ name: 'worlds', active: false }, () => p.setDimension(1));
    }, /stopped/);
    f.host.disconnect(7);
    assert.throws(() => p.setDimension(1), /Unknown player/);
    assert.equal(f.output.filter((e) => e.op === 'entityWorld').length, 3);
    // A late result for a previous incarnation never touches a reconnected player.
    const replacement = player(f.host, 7);
    f.host.confirmPlayerWorld({ id: 7, request: 3, dimension: 1 });
    assert.equal(replacement.dimension, 0xffffffff);
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});

test('Player.showOnlyTo accepts a connected Player or null and rejects everything else', async () => {
  const f = fixture();
  resource(
    f.root,
    'private',
    `'use strict';
    kcds.core.Events.on('playerCommand', (p, c, args) => {
      const target = kcds.players.Player.getById(Number(args[0]));
      if (c === 'show') {
        p.showOnlyTo(target);
      }
      if (c === 'clear') {
        p.showOnlyTo(null);
      }
      if (c === 'invalid') {
        const outcomes = [];
        const horse = kcds.characters.Horse.spawn(p.position);
        for (const value of [undefined, 7, {}, { id: 8 }, horse]) {
          try {
            p.showOnlyTo(value);
            outcomes.push('accepted');
          } catch (error) {
            outcomes.push(error.constructor.name);
          }
        }
        horse.destroy();
        kcds.core.Chat.sendToPlayer(p, outcomes.join(','));
      }
    });`,
  );
  try {
    await f.host.startAll();
    const p = player(f.host);
    const other = player(f.host, 8);

    f.host.receiveChat(p.id, '/show 8');
    f.host.receiveChat(p.id, '/show 7');
    f.host.receiveChat(p.id, '/clear');
    assert.deepEqual(
      f.output.filter((e) => e.op === 'entityVisibleTo').map((e) => ({ ...e })),
      [
        { op: 'entityVisibleTo', id: 7, playerId: 8 },
        { op: 'entityVisibleTo', id: 7, playerId: 7 },
        { op: 'entityVisibleTo', id: 7, playerId: 0 },
      ],
    );

    f.host.receiveChat(p.id, '/invalid');
    assert.equal(f.output.at(-1).text, 'TypeError,TypeError,TypeError,TypeError,TypeError');
    assert.equal(f.output.filter((e) => e.op === 'entityVisibleTo').length, 3);

    // A disconnected target or a stale subject handle is rejected before the bridge.
    f.host.disconnect(8);
    assert.throws(() => p.showOnlyTo(other), /Unknown player/);
    assert.throws(() => other.showOnlyTo(null), /Unknown player/);

    await f.host.stop('private');
    assert.throws(() => {
      f.host.context.run({ name: 'private', active: false }, () => p.showOnlyTo(null));
    }, /stopped/);
    assert.equal(f.output.filter((e) => e.op === 'entityVisibleTo').length, 3);
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});

test('a horse is a full Entity with a reserved network id and live transform', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-resource-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 500, count: 1025 },
    send: (e) => output.push(e),
    log() {},
  });
  resource(
    root,
    'stable',
    `kcds.core.Events.on('playerCommand', (p, c) => {
      if (c !== 'horse') return;
      const h = kcds.characters.Horse.spawn({ x: 1, y: 2, z: 3 });
      h.position = { x: 4, y: 5, z: 6 };
      h.rotation = { x: 0, y: 0, z: 90 };
      h.showOnlyTo(p);
      h.showOnlyTo(null);
      const results = [h.id, h.dimension, h instanceof kcds.players.Entity];
      h.dimension = 7;
      results.push(h.dimension);
      h.setDimension(7);
      results.push(h.dimension);
      h.destroy();
      try {
        h.position = { x: 0, y: 0, z: 0 };
        results.push('accepted');
      } catch (error) {
        results.push(error.message);
      }
      kcds.core.Chat.sendToPlayer(p, JSON.stringify(results));
    });`,
  );
  await host.startAll();
  const p = player(host);
  host.emit('playerCommand', p, 'horse', []);
  const chat = output.find((e) => e.op === 'chat');
  assert.deepEqual(JSON.parse(chat.text), [500, 0xffffffff, true, 0xffffffff, 7, 'Unknown horse']);
  const spawn = output.find((e) => e.op === 'horseSpawn');
  assert.equal(spawn.id, 500);
  assert.deepEqual(
    output.find((e) => e.op === 'entityPosition'),
    { op: 'entityPosition', id: 500, position: { x: 4, y: 5, z: 6 } },
  );
  const turn = output.find((e) => e.op === 'entityRotation');
  assert.equal(turn.id, 500);
  assert.ok(
    Math.abs(Math.hypot(turn.rotation.x, turn.rotation.y, turn.rotation.z, turn.rotation.w) - 1) <
      1e-9,
  );
  assert.deepEqual(
    output.filter((e) => e.op === 'entityVisibleTo').map((e) => e.playerId),
    [p.id, 0],
  );
  // The native report updates the handle; ids come from the reserved block, a refill is asked for
  // once fewer than 1024 remain, and spawning stops when the blocks run out. The horse took two
  // (itself, 500, and its saddlebags, 501).
  assert.equal(host.allocateEntityId(), 502);
  assert.equal(output.filter((e) => e.op === 'entityIdsRequest').length, 1);
  host.addEntityIds({ first: 9000, count: 2 });
  for (let i = 503; i < 1525; i += 1) host.allocateEntityId();
  assert.equal(host.allocateEntityId(), 9000);
  assert.equal(host.allocateEntityId(), 9001);
  assert.throws(() => host.allocateEntityId(), /No entity ids available/);
});

test('player lifecycle events fire once each, in contract order', async () => {
  const f = fixture();
  resource(
    f.root,
    'lifecycle',
    `for (const name of ['playerConnect', 'playerSpawning', 'playerSpawned', 'playerReady']) {
      kcds.core.Events.on(name, (p) => kcds.core.Chat.sendToAll(name + ':' + p.id));
    }`,
  );
  await f.host.startAll();
  const report = (extra) =>
    f.host.updatePlayer({
      id: 9,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      ...extra,
    });
  report({ ready: false, gameplayReady: false });
  report({ ready: false, gameplayReady: false });
  report({ ready: true, gameplayReady: false });
  report({ ready: true, gameplayReady: true });
  report({ ready: true, gameplayReady: true });
  const events = f.output.filter((e) => e.op === 'chat').map((e) => e.text);
  assert.deepEqual(events, [
    'playerConnect:9',
    'playerSpawning:9',
    'playerSpawned:9',
    'playerReady:9',
  ]);
});

test('an admitted identity becomes the identity of its player', async () => {
  const f = fixture();
  await f.host.startAll();
  const deviceId = 'ab'.repeat(32);
  const identity = {
    nickname: 'Henry',
    steamId: '76561198000000009',
    discordId: '1563839669496578088',
    deviceId,
    ip: '127.0.0.1',
  };
  assert.deepEqual(await f.host.admitConnection(11, identity), { allowed: true, reason: '' });
  f.host.playerAdmitted(11, 9);
  const henry = player(f.host, 9);
  // A pass without a Steam identity (this fixture's gate): the client's Steam id is not taken as
  // verified.
  assert.equal(henry.steamId, '');
  assert.equal(henry.store, '');
  assert.equal(henry.storeAccount, '');
  assert.equal(henry.discordId, '1563839669496578088');
  // Reported by the client, not from a pass.
  assert.equal(henry.discordVerified, false);
  assert.equal(henry.deviceId, deviceId);
  // The address the server saw the connection come from.
  assert.equal(henry.ip, '127.0.0.1');
  // The verified Steam identity of a licensed join.
  f.host.playerIdentities.set(
    10,
    Object.freeze({
      ...f.host.identityOf(9),
      steamId: '7656',
      store: 'steam',
      storeAccount: '7656',
    }),
  );
  const anna = player(f.host, 10);
  assert.equal(anna.steamId, '7656');
  assert.equal(anna.store, 'steam');
  assert.throws(() => {
    anna.steamId = 'forged';
  });
  // Every identifier with whether the server verified it.
  assert.deepEqual(henry.identifiers, [
    { kind: 'discord', id: '1563839669496578088', verified: false },
    { kind: 'hardware', id: deviceId, verified: false },
    { kind: 'ip', id: '127.0.0.1', verified: true },
  ]);
  assert.deepEqual(
    anna.identifiers.map((identifier) => `${identifier.kind}:${identifier.verified}`),
    ['steam:true', 'discord:false', 'hardware:false', 'ip:true'],
  );
  assert.equal(anna.getIdentifier('steam'), '7656');
  assert.equal(anna.getIdentifier('epic'), '');
  assert.equal(henry.getIdentifier('hardware'), deviceId);
  assert.throws(() => henry.getIdentifier('passport'), TypeError);
  assert.ok(Object.isFrozen(anna.identifiers) && Object.isFrozen(anna.identifiers[0]));
  // getIP and the transport's round-trip time.
  assert.equal(henry.getIP(), '127.0.0.1');
  assert.equal(henry.ping, 0);
  f.host.setPings([
    [9, 42.4],
    [10, -1],
    ['x', 5],
  ]);
  assert.equal(henry.ping, 42);
  assert.equal(anna.ping, 0);
  // A player who never went through playerConnecting has none; a leaving player's goes.
  assert.equal(player(f.host, 12).discordId, '');
  f.host.disconnect(9);
  assert.equal(f.host.identityOf(9).discordId, '');
  assert.equal(f.host.admittedIdentities.size, 0);
});

test('playerConnecting admits after every handler settled and refuses on a failure', async () => {
  const empty = fixture();
  await empty.host.startAll();
  assert.deepEqual(await empty.host.admitConnection(1), { allowed: true, reason: '' });
  const f = fixture();
  resource(
    f.root,
    'gate',
    `let calls = 0;
    kcds.core.Events.on('playerConnecting', async (connection) => {
      calls += 1;
      await new Promise((resolve) => setTimeout(resolve, 20));
      if (calls === 2) throw new Error('whitelist');
    });`,
  );
  await f.host.startAll();
  assert.deepEqual(await f.host.admitConnection(2), { allowed: true, reason: '' });
  // A thrown error turns the player away without showing them its message.
  assert.deepEqual(await f.host.admitConnection(3), { allowed: false, reason: '' });
  assert.ok(f.logs.some((entry) => String(entry).includes('playerConnecting refused request 3')));
});

test('connection.reject turns the player away at once with its reason', async () => {
  const f = fixture();
  resource(
    f.root,
    'gate',
    `kcds.core.Events.on('playerConnecting', (connection) => {
      connection.reject('Whitelist only. Apply on our Discord.');
      connection.reject('a second call changes nothing');
    });
    kcds.core.Events.on('playerConnecting', () => new Promise(() => {}));
    kcds.core.Events.on('playerConnecting', async () => {
      await new Promise((resolve) => setTimeout(resolve, 5));
      throw new Error('late failure');
    });`,
  );
  await f.host.startAll();
  // Decided although one handler never settles and another fails afterwards.
  assert.deepEqual(await f.host.admitConnection(4), {
    allowed: false,
    reason: 'Whitelist only. Apply on our Discord.',
  });
  await new Promise((resolve) => setTimeout(resolve, 20));
});

test('a waiting connection has its identifiers to ban on', async () => {
  const f = fixture();
  const banned = 'cd'.repeat(32);
  resource(
    f.root,
    'bans',
    `const bannedMachines = new Set(['${banned}']);
    kcds.core.Events.on('playerConnecting', (connection) => {
      const kinds = connection.identifiers.map((identifier) => identifier.kind).join(',');
      connection.update('identifiers ' + kinds);
      if (connection.ticket !== 'signed.pass') {
        connection.reject('no ticket');
      }
      if (bannedMachines.has(connection.getIdentifier('hardware'))) {
        connection.reject('This machine is banned here.');
      }
    });`,
  );
  await f.host.startAll();
  const identity = {
    nickname: 'Henry',
    discordId: '15',
    deviceId: banned,
    ip: '10.0.0.2',
    licence: 'signed.pass',
  };
  assert.deepEqual(await f.host.admitConnection(21, identity), {
    allowed: false,
    reason: 'This machine is banned here.',
  });
  const other = { ...identity, deviceId: 'ef'.repeat(32) };
  assert.deepEqual(await f.host.admitConnection(22, other), { allowed: true, reason: '' });
});

// the launcher's join token reaches playerConnecting, '' without one.
test('connection.joinToken is what the launcher gave the join', async () => {
  const f = fixture();
  resource(
    f.root,
    'launcher',
    `kcds.core.Events.on('playerConnecting', (connection) => {
      if (connection.joinToken !== 'one-time.ticket_42') {
        connection.reject('token ' + JSON.stringify(connection.joinToken));
      }
    });`,
  );
  await f.host.startAll();
  const identity = { nickname: 'Henry', joinToken: 'one-time.ticket_42' };
  assert.deepEqual(await f.host.admitConnection(31, identity), { allowed: true, reason: '' });
  assert.deepEqual(await f.host.admitConnection(32, { nickname: 'Hans' }), {
    allowed: false,
    reason: 'token ""',
  });
});

test('connection.reject cuts its reason to 512 bytes and checks its type', async () => {
  const f = fixture();
  resource(
    f.root,
    'gate',
    `kcds.core.Events.on('playerConnecting', (connection) => {
      let typeError = '';
      try {
        connection.reject(42);
      } catch (error) {
        typeError = error.name;
      }
      connection.reject(typeError + ' ' + 'a'.repeat(495) + 'ěěěěěěěěěě');
    });`,
  );
  await f.host.startAll();
  const result = await f.host.admitConnection(5);
  assert.equal(result.allowed, false);
  assert.ok(result.reason.startsWith('TypeError '));
  // 9 + 1 + 495 ASCII bytes and three 2-byte characters make 511; a fourth would split at 512.
  assert.equal(Buffer.byteLength(result.reason), 511);
  assert.ok(result.reason.endsWith('aěěě'));
});

test('connection.reject without a reason refuses with an empty one', async () => {
  const f = fixture();
  resource(
    f.root,
    'gate',
    `kcds.core.Events.on('playerConnecting', (connection) => connection.reject());`,
  );
  await f.host.startAll();
  assert.deepEqual(await f.host.admitConnection(6), { allowed: false, reason: '' });
});
test('playerDied names the killer and reason of the health taken just before it', async () => {
  const f = fixture();
  resource(
    f.root,
    'deaths',
    `'use strict';
    kcds.core.Events.on('playerDied', (p, killer, reason) => {
      kcds.core.Chat.sendToAll('died ' + p.id + ' ' + (killer ? killer.id : 'null') + ' ' + reason);
    });`,
  );
  try {
    await f.host.startAll();
    const victim = player(f.host, 7);
    player(f.host, 8);
    // A blow from player 8, then the knock-out it causes.
    f.host.playerDamage({ id: 7, attacker: 8, amount: 40, reason: 1, subpart: -1 });
    f.host.playerDied(7);
    victim.revive();
    // A bleed-out: its killing tick, no attacker.
    f.host.playerDamage({ id: 7, attacker: 0, amount: 1, reason: 9, subpart: -1 });
    f.host.playerDied(7);
    const died = f.output.filter((e) => e.op === 'chat').map((e) => e.text);
    assert.deepEqual(
      died.filter((t) => t.startsWith('died')),
      ['died 7 8 combat', 'died 7 null bleeding'],
    );
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});
test('playerDied reports a knock-out once; revive and setKnockoutVision reach the bridge', async () => {
  const f = fixture();
  resource(
    f.root,
    'knockout',
    `'use strict';
    kcds.core.Events.on('playerDied', (p, killer, reason) => {
      kcds.core.Chat.sendToPlayer(p, 'died ' + String(killer) + ' ' + reason);
    });
    kcds.core.Events.on('playerCommand', (p, c, args) => {
      if (c === 'revive') {
        kcds.core.Chat.sendToPlayer(p, 'revive ' + p.revive());
      }
      if (c === 'vision') {
        const outcomes = [];
        for (const value of [0, 40, 100, -1, 101, 2.5, '50', undefined]) {
          try {
            p.setKnockoutVision(value);
            outcomes.push('accepted');
          } catch (error) {
            outcomes.push(error.constructor.name);
          }
        }
        kcds.core.Chat.sendToPlayer(p, outcomes.join(','));
      }
    });`,
  );
  try {
    await f.host.startAll();
    const p = player(f.host);

    // Not down: nothing to revive.
    f.host.receiveChat(p.id, '/revive');
    assert.equal(f.output.at(-1).text, 'revive false');

    // A knock-out is raised once, until the player is revived.
    assert.equal(f.host.playerDied(p.id), true);
    assert.equal(f.host.playerDied(p.id), false);
    assert.equal(f.output.filter((e) => e.text === 'died null unknown').length, 1);

    f.host.receiveChat(p.id, '/revive');
    assert.equal(f.output.at(-1).text, 'revive true');
    f.host.receiveChat(p.id, '/revive');
    assert.equal(f.output.at(-1).text, 'revive false');
    assert.deepEqual(
      f.output.filter((e) => e.op === 'revive').map((e) => ({ ...e })),
      [{ op: 'revive', id: 7 }],
    );

    f.host.receiveChat(p.id, '/vision');
    assert.equal(
      f.output.at(-1).text,
      'accepted,accepted,accepted,RangeError,RangeError,RangeError,RangeError,RangeError',
    );
    assert.deepEqual(
      f.output.filter((e) => e.op === 'knockoutVision').map((e) => ({ ...e })),
      [
        { op: 'knockoutVision', id: 7, percent: 0 },
        { op: 'knockoutVision', id: 7, percent: 40 },
        { op: 'knockoutVision', id: 7, percent: 100 },
      ],
    );
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});

test('knockOut asks the game for a knock-out, and knock-outs are numbered episodes', async () => {
  const f = fixture();
  resource(
    f.root,
    'episodes',
    `'use strict';
    kcds.core.Events.on('playerDied', (p, killer, reason, episode) => {
      kcds.core.Chat.sendToPlayer(p, ['died', reason, episode, p.knockoutEpisode].join(' '));
    });
    kcds.core.Events.on('playerRevived', (p, episode) => {
      kcds.core.Chat.sendToPlayer(p, 'revived ' + episode + ' ' + p.knockoutEpisode);
    });
    kcds.core.Events.on('playerCommand', (p, c, args) => {
      if (c === 'ko') {
        const options = args[0] === 'keep' ? { health: 5, keepEffects: true } : {};
        kcds.core.Chat.sendToPlayer(p, 'ko ' + p.knockOut(options));
      }
      if (c === 'revive') {
        kcds.core.Chat.sendToPlayer(p, 'revive ' + p.revive({ episode: Number(args[0]) }));
      }
      if (c === 'bad') {
        const outcomes = [];
        for (const options of [{ health: 0 }, { health: Infinity }, { health: '5' }, { keepEffects: 1 }, null]) {
          try {
            p.knockOut(options);
            outcomes.push('accepted');
          } catch (error) {
            outcomes.push(error.constructor.name);
          }
        }
        kcds.core.Chat.sendToPlayer(p, outcomes.join(','));
      }
    });`,
  );
  try {
    await f.host.startAll();
    const p = player(f.host);
    const said = () => f.output.filter((e) => typeof e.text === 'string').at(-1)?.text;
    assert.equal(p.knockoutEpisode, 0);
    f.host.receiveChat(p.id, '/bad');
    assert.equal(said(), 'RangeError,TypeError,TypeError,TypeError,TypeError');

    // The cures, then the knock-out; playerDied names it and numbers it.
    f.host.receiveChat(p.id, '/ko');
    assert.equal(said(), 'ko true');
    const knockOuts = () => f.output.filter((e) => e.op === 'knockOut').map((e) => ({ ...e }));
    assert.deepEqual(knockOuts(), [{ op: 'knockOut', id: 7, health: 1 }]);
    assert.equal(f.host.playerDied(p.id), true);
    assert.equal(said(), 'died knockOut 1 1');
    // Down: no second knock-out; a revive of another episode does nothing.
    f.host.receiveChat(p.id, '/ko');
    assert.equal(said(), 'ko false');
    f.host.receiveChat(p.id, '/revive 2');
    assert.equal(said(), 'revive false');
    f.host.receiveChat(p.id, '/revive 1');
    assert.equal(f.output.filter((e) => e.text === 'revived 1 0').length, 1);
    assert.equal(f.output.filter((e) => e.op === 'revive').length, 1);
    // The same episode again: the player is up, nothing happens.
    f.host.receiveChat(p.id, '/revive 1');
    assert.equal(said(), 'revive false');

    // A lethal hit is the next episode, with its own reason.
    assert.equal(f.host.playerDied(p.id), true);
    assert.equal(said(), 'died unknown 2 2');
    f.host.receiveChat(p.id, '/revive 2');
    f.host.receiveChat(p.id, '/ko keep');
    assert.deepEqual(knockOuts().at(-1), { op: 'knockOut', id: 7, health: 5 });
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});

test('bundled Node code finds global, process and setImmediate in a server resource', async (t) => {
  const f = fixture();
  t.after(async () => {
    await f.host.shutdown();
    fs.rmSync(f.root, { recursive: true, force: true });
  });
  resource(
    f.root,
    'bundle',
    `
    const seen = [];
    global.shimmed = 1;
    seen.push(typeof global, global === globalThis, globalThis.shimmed, global.kcds === kcds);
    seen.push(typeof process.env, process.platform, typeof process.versions.node);
    seen.push(typeof process.exit, typeof process.on, process.cwd() === __dirname);
    process.nextTick((value) => seen.push('tick ' + value), 1);
    setImmediate((value) => {
      seen.push('immediate ' + value);
      console.log(JSON.stringify(seen));
    }, 2);
    `,
  );
  await f.host.startAll();
  await new Promise((resolve) => setTimeout(resolve, 50));
  const line = f.logs.find((entry) => entry[0] === 'bundle' && entry[1].startsWith('['));
  assert.ok(line, JSON.stringify(f.logs));
  assert.deepEqual(JSON.parse(line[1]), [
    'object',
    true,
    1,
    true,
    'object',
    process.platform,
    'string',
    'undefined',
    'undefined',
    true,
    'tick 1',
    'immediate 2',
  ]);
});
