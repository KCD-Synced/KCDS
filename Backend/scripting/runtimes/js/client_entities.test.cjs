'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { ClientEntities } = require('../../../../Client/resources/entities.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');
const { normalizeManifest } = require('./manifest.cjs');

function directory(nativeId = 1) {
  return {
    localPlayerId: 1,
    entities: [
      {
        id: 1,
        nativeId,
        kind: 'player',
        dimension: 0xffffffff,
        position: { x: 1, y: 2, z: 3 },
        rotation: { w: 1, x: 0, y: 0, z: 0 },
      },
    ],
  };
}

test('LocalPlayer.knockedOut follows the native client; other players read false', () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  const next = directory();
  next.entities.push({ ...next.entities[0], id: 2, nativeId: 2 });
  registry.updateDirectory(next);
  assert.equal(api.LocalPlayer.knockedOut, false);
  assert.equal(registry.receiveLocalKnockout({ down: true }), 'localPlayerKnockedOut');
  assert.equal(registry.receiveLocalKnockout({ down: true }), null);
  assert.equal(registry.receiveLocalKnockout({ down: 1 }), null);
  assert.equal(api.LocalPlayer.knockedOut, true);
  assert.equal(new api.Player(2).knockedOut, false);
  assert.equal(registry.receiveLocalKnockout({ down: false }), 'localPlayerRevived');
  assert.equal(api.LocalPlayer.knockedOut, false);
  registry.receiveLocalKnockout({ down: true });
  registry.disconnect();
  assert.equal(registry.localDown, false);
});

test('client native directory supplies live LocalPlayer, immutable IDs and copied transforms', () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  assert.equal(api.LocalPlayer, null);
  registry.updateDirectory(directory());
  const player = api.LocalPlayer;
  assert.equal(player.id, 1);
  assert.equal(player.dimension, 0xffffffff);
  assert.equal(player.toString(), 'Player(1, "", local)');
  assert.ok(player instanceof api.Player);
  assert.ok(player instanceof api.Entity);
  assert.throws(() => {
    player.id = 2;
  }, TypeError);
  player.position.x = 99;
  assert.equal(player.position.x, 1);
  const next = directory();
  next.entities[0].position.x = 4;
  registry.updateDirectory(next);
  assert.equal(player.position.x, 4);
  assert.throws(() => new api.Entity(2), /Unknown/);
  assert.throws(() => new api.Entity(-1), /Invalid/);
});

test('a player carries the appearance and disguise its own client published', () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  registry.updateDirectory(directory());
  const player = new api.Player(1);
  // A server that does not report them: the default body, no disguise.
  assert.deepEqual(
    { ...player.appearance },
    { gender: 'male', body: '', head: '', hair: '', beard: '' },
  );
  assert.equal(player.disguise, null);
  const next = directory();
  next.entities[0].appearance = {
    gender: 'male',
    body: 'm_body_tan_03',
    head: 'm_head_004',
    hair: 'm_hair_005_ginger',
    beard: 'UC_beard_radzig',
  };
  next.entities[0].disguise = 'objects/manmade/barrels/barrel_a.cgf';
  next.entities.push({
    id: 2,
    nativeId: 2,
    kind: 'horse',
    dimension: 0,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  registry.updateDirectory(next);
  assert.equal(player.appearance.head, 'm_head_004');
  assert.ok(Object.isFrozen(player.appearance));
  assert.equal(player.disguise, 'objects/manmade/barrels/barrel_a.cgf');
  next.entities[0].disguise = '';
  registry.updateDirectory(next);
  assert.equal(player.disguise, null);
  // Only players are players; a malformed look is refused with the whole directory.
  assert.throws(() => new api.Player(2), /Unknown player/);
  const bad = directory();
  bad.entities[0].appearance = { gender: 'dwarf', body: '', head: '', hair: '', beard: '' };
  assert.throws(() => registry.updateDirectory(bad), TypeError);
  assert.equal(player.appearance.head, 'm_head_004');
});

test('a player carries what its body wears and holds drawn', () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  registry.updateDirectory(directory());
  const player = new api.Player(1);
  assert.deepEqual(player.equipment, []);
  assert.equal(player.rightHandItem, '');
  assert.equal(player.leftHandItem, '');
  const sword = '46db8ef200cca9e319099351ec86bf8c';
  const next = directory();
  next.entities[0].equipment = [sword];
  next.entities[0].hands = ['', sword];
  registry.updateDirectory(next);
  const worn = player.equipment;
  worn.push('changed');
  assert.deepEqual(player.equipment, [sword]);
  assert.equal(player.leftHandItem, sword);
  const bad = directory();
  bad.entities[0].hands = [sword];
  assert.throws(() => registry.updateDirectory(bad), /Invalid native player hands/);
  bad.entities[0].hands = undefined;
  bad.entities[0].equipment = ['46DB8EF200CCA9E319099351EC86BF8C'];
  assert.throws(() => registry.updateDirectory(bad), /Invalid native player equipment/);
  assert.equal(player.leftHandItem, sword);
});

test('directory validation is atomic and stale native incarnations cannot write state', () => {
  const registry = new ClientEntities();
  registry.updateDirectory(directory());
  const player = registry.facade().LocalPlayer;
  const bad = directory();
  bad.entities[0].position.y = NaN;
  assert.throws(() => registry.updateDirectory(bad), /transform/);
  assert.equal(player.position.y, 2);
  registry.applyState({
    id: 1,
    nativeId: 1,
    entries: [{ key: 'score', kind: 'number', value: 3 }],
  });
  const bag = player.state;
  registry.updateDirectory(directory(2));
  assert.throws(() => player.position, /no longer/);
  assert.throws(() => bag.get('score'), /no longer/);
  assert.equal(registry.applyState({ id: 1, nativeId: 1, entries: [] }), false);
  assert.equal(registry.facade().LocalPlayer.state.get('score'), undefined);
});

test('force requests carry only scalar identity and canonical transforms and reject stopped resources', () => {
  const requests = [];
  const owner = { active: true };
  const registry = new ClientEntities({
    force: (request) => requests.push(request),
    ownerAlive: (value) => value?.active,
  });
  registry.updateDirectory(directory());
  const player = registry.facade(owner).LocalPlayer;
  player.position = { x: 5, y: 6, z: 7 };
  player.rotation = { x: 0, y: 0, z: 90 };
  assert.equal(requests[0].id, 1);
  assert.equal(requests[0].nativeId, 1);
  assert.equal(requests[0].value.x, 5);
  assert.ok(Math.abs(requests[1].value.z - Math.SQRT1_2) < 1e-9);
  assert.equal(player.position.x, 1);
  owner.active = false;
  assert.throws(() => {
    player.position = {};
  }, /stopped/);
  registry.disconnect();
  assert.equal(registry.facade().LocalPlayer, null);
  assert.throws(() => player.state, /no longer/);
});

test('actual browser resource globals bind Entity/state events and reload removes listeners but retains entity data', async (t) => {
  const messages = [];
  const runtime = new BrowserResourceRuntime({ send: (value) => messages.push(value) });
  t.after(() => runtime.disconnect());
  runtime.entities.updateDirectory(directory());
  const source = `
    const player = kcds.players.LocalPlayer;
    if (!(player instanceof kcds.players.Entity) || !(player.state instanceof kcds.core.SharedState)) {
      throw Error('missing binding');
    }
    player.state.onChange('score', (key, value) => {
      kcds.core.Events.toServer('score', value);
    });
    kcds.core.Events.on('entityStateChange', (entity, key, value) => {
      if (!(entity instanceof kcds.players.Entity)) {
        throw Error('event handle is not owned by resource');
      }
      if (entity.id === player.id) {
        kcds.core.Events.toServer('global', value);
      }
      if (key === 'score' && value === 2) {
        entity.state.onChange('later', () => {
          kcds.core.Events.toServer('late');
        });
      }
    });
  `;
  const definition = {
    hash: 'v1',
    manifest: normalizeManifest({ name: 'entities', kcds: { clientScripts: ['client.cjs'] } }),
    files: [{ name: 'client.cjs' }],
  };
  const read = () => new TextEncoder().encode(source);
  await runtime.apply({ resources: [definition] }, read);
  runtime.entities.applyState({
    id: 1,
    nativeId: 1,
    entries: [{ key: 'score', kind: 'number', value: 2 }],
  });
  assert.deepEqual(
    messages.map((value) => value.name),
    ['score', 'global'],
  );
  await runtime.stop('entities');
  runtime.entities.applyState({
    id: 1,
    nativeId: 1,
    entries: [{ key: 'score', kind: 'number', value: 3 }],
  });
  runtime.entities.applyState({
    id: 1,
    nativeId: 1,
    entries: [
      { key: 'score', kind: 'number', value: 3 },
      { key: 'later', kind: 'number', value: 1 },
    ],
  });
  assert.equal(messages.length, 2);
  await runtime.apply({ resources: [{ ...definition, hash: 'v2' }] }, read);
  assert.equal(runtime.entities.facade().LocalPlayer.state.get('score'), 3);
  runtime.entities.applyState({
    id: 1,
    nativeId: 1,
    entries: [
      { key: 'score', kind: 'number', value: 4 },
      { key: 'later', kind: 'number', value: 1 },
    ],
  });
  assert.equal(messages.length, 4);
  await runtime.disconnect();
  assert.equal(runtime.entities.facade().LocalPlayer, null);
});

test("a client's Player reads the nameplate the server set", () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  registry.updateDirectory(directory());
  const player = new api.Player(1);
  // Nothing set: the plain tag.
  assert.equal(player.isNameplateVisible(), true);
  assert.equal(player.isNameplateHealthVisible(), true);
  assert.equal(player.isNameplateNameVisible(), true);
  assert.equal(player.getNameplateText(), '');
  assert.equal(player.getNameplateColor(), 0xffffffff);
  assert.equal(player.getNameplateTitle(), '');
  const next = directory();
  next.entities[0].nameplate = {
    visible: true,
    health: false,
    name: true,
    text: '[Guard] Hans',
    color: 0xffc9a227,
    title: 'Town guard',
    titleColor: 0,
    titleSize: 0,
  };
  registry.updateDirectory(next);
  assert.equal(player.isNameplateHealthVisible(), false);
  assert.equal(player.getNameplateText(), '[Guard] Hans');
  assert.equal(player.getNameplateColor(), 0xffc9a227);
  assert.equal(player.getNameplateTitle(), 'Town guard');
  const broken = directory();
  broken.entities[0].nameplate = { visible: 'yes' };
  assert.throws(() => registry.updateDirectory(broken), /nameplate/);
});

// Player.visible on the client: whether the body is drawn, as its own client publishes.
test('a player is visible until the directory says its body is not drawn', () => {
  const registry = new ClientEntities();
  const api = registry.facade();
  registry.updateDirectory(directory());
  const player = new api.Player(1);
  assert.equal(player.visible, true, 'a server that does not report it');
  const hidden = directory();
  hidden.entities[0].visible = false;
  registry.updateDirectory(hidden);
  assert.equal(player.visible, false);
  hidden.entities[0].visible = true;
  registry.updateDirectory(hidden);
  assert.equal(player.visible, true);
  const bad = directory();
  bad.entities[0].visible = 'no';
  assert.throws(() => registry.updateDirectory(bad), /visibility/);
  assert.equal(player.visible, true);
});

// Player.setLockable: this machine's lock-on passes over the player.
test('setLockable tells the game client which player to keep out of the lock-on', () => {
  const sent = [];
  const registry = new ClientEntities({ send: (message) => sent.push(message) });
  const api = registry.facade();
  registry.updateDirectory(directory());
  const player = new api.Player(1);
  player.setLockable(false);
  player.setLockable(true);
  assert.deepEqual(sent, [
    { op: 'playerTargetable', id: 1, targetable: false },
    { op: 'playerTargetable', id: 1, targetable: true },
  ]);
  assert.throws(() => player.setLockable('no'), TypeError);
  assert.throws(() => player.setLockable(), TypeError);
});

// where this machine's game draws a player's body and its joints.
test('renderPosition and jointPosition ask the game client where the body is drawn', () => {
  const asked = [];
  const registry = new ClientEntities({
    pose: (entity, joint) => {
      asked.push([entity, joint]);
      if (joint === 'Nose') return null;
      return { p: joint ? [1, 2, 3.5] : [1, 2, 2] };
    },
  });
  const api = registry.facade();
  registry.updateDirectory(directory());
  const player = new api.Player(1);
  assert.equal(player.renderPosition, null);
  registry.updateEngineIds({ ids: [[1, 30583]] });
  assert.deepEqual({ ...player.renderPosition }, { x: 1, y: 2, z: 2 });
  assert.deepEqual({ ...player.jointPosition('Bip01 Head') }, { x: 1, y: 2, z: 3.5 });
  assert.equal(player.jointPosition('Nose'), null);
  assert.deepEqual(asked, [
    [30583, ''],
    [30583, 'Bip01 Head'],
    [30583, 'Nose'],
  ]);
  assert.throws(() => player.jointPosition(''), TypeError);
  assert.throws(() => player.jointPosition(7), TypeError);
});

// how a joint of a player's body is turned, from the same answer as its position.
test('jointRotation reads the turn of a joint the game client draws', () => {
  const registry = new ClientEntities({
    pose: (entity, joint) => {
      if (joint === 'Nose') return null;
      return { p: [1, 2, 3.5], q: [0, 0, 0.7071, 0.7071] };
    },
  });
  const api = registry.facade();
  registry.updateDirectory(directory());
  const player = new api.Player(1);
  assert.equal(player.jointRotation('RightHand'), null, 'no body here yet');
  registry.updateEngineIds({ ids: [[1, 30583]] });
  const turn = player.jointRotation('RightHand');
  assert.deepEqual([turn.w, turn.x, turn.y, turn.z], [0.7071, 0, 0, 0.7071]);
  assert.equal(player.jointRotation('Nose'), null);
  assert.throws(() => player.jointRotation(''), TypeError);
  assert.throws(() => player.jointRotation(null), TypeError);
});

// a client script takes a player's body here out of the collisions.
test('setCollision asks the game client to let a body be walked through', () => {
  const sent = [];
  const registry = new ClientEntities({ send: (message) => sent.push(message) });
  const api = registry.facade();
  registry.updateDirectory(directory());
  const player = new api.Player(1);
  player.setCollision(false);
  player.setCollision(true);
  assert.deepEqual(sent, [
    { op: 'playerCollision', id: 1, collides: false },
    { op: 'playerCollision', id: 1, collides: true },
  ]);
  assert.throws(() => player.setCollision('no'), TypeError);
});
