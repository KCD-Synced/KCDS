'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { lookupItemClass, lookupOutfit, outfitNames } = require('./inventory/catalog.cjs');

// Npc: what a resource's calls send the native server and what its reports
// change here.

const SOUL = '210170d8-afb2-4cbd-9df5-da2a8e722af4';

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-npcs-'));
  const output = [];
  const logs = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: (owner, text) => logs.push(text),
  });
  await host.startAll();
  const sent = (op) => output.filter((message) => message.op === op);
  return { host, Npc: host.npcs.Npc, sent, logs };
}

test('create sends the spawn and makes the NPC findable', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ soul: SOUL, name: 'Citatel', position: { x: 1, y: 2, z: 3 } });
  const [spawn] = sent('npcSpawn');
  assert.equal(spawn.id, npc.id);
  assert.equal(spawn.soul, SOUL);
  assert.equal(spawn.actorClass, 'NPC');
  assert.equal(spawn.name, 'Citatel');
  assert.deepEqual(spawn.position, { x: 1, y: 2, z: 3 });
  assert.equal(Npc.getById(npc.id), npc);
  assert.deepEqual(Npc.all(), [npc]);
  assert.throws(() => Npc.create({ soul: 'nobody', position: { x: 0, y: 0, z: 0 } }), RangeError);
  assert.throws(
    () => Npc.create({ position: { x: 0, y: 0, z: 0 }, maxHealth: 50 }),
    /comes in a later KCDS phase/,
  );
  assert.throws(() => Npc.create({ position: { x: 0, y: 0, z: 0 }, locomotion: 'fly' }), TypeError);
  assert.throws(() => Npc.create({ position: { x: 0, y: 0, z: 0 }, faction: -1 }), RangeError);
  assert.throws(() => Npc.create({ position: { x: 0, y: 0, z: 0 }, nameplate: 'yes' }), TypeError);
  assert.equal(sent('npcSpawn').length, 1, 'a refused option spawns nothing');
  // The maximum health every NPC has is accepted.
  const full = Npc.create({ position: { x: 0, y: 0, z: 0 }, maxHealth: 100 });
  assert.equal(full.maxHealth, 100);
});

test('playAnimation sends the clip with a new sequence each time; stopAnimation clears it', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.playAnimation('', { clip: 'reading_stand_inv_in', loop: true }), true);
  assert.equal(npc.playAnimation('', { clip: 'Animations/x/wave.caf' }), true);
  assert.equal(npc.stopAnimation(), true);
  assert.deepEqual(
    sent('npcAnimation').map((message) => [message.id, message.animation]),
    [
      [npc.id, '1|1|reading_stand_inv_in'],
      [npc.id, '2|0|Animations/x/wave.caf'],
      [npc.id, ''],
    ],
  );
});

test('playAnimation sends a fragment with its tags after an empty clip', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.playAnimation('GetMash', { tags: 'mash+sitting' }), true);
  assert.equal(npc.playAnimation('PrayStand', { loop: true }), true);
  // A clip wins over the fragment's name.
  assert.equal(npc.playAnimation('Wave', { clip: 'relaxed_idle_wave' }), true);
  assert.deepEqual(
    sent('npcAnimation').map((message) => message.animation),
    ['1|0||GetMash|mash+sitting', '2|1||PrayStand|', '3|0|relaxed_idle_wave'],
  );
  assert.throws(() => npc.playAnimation('Get|Mash'), RangeError);
  assert.throws(() => npc.playAnimation('x'.repeat(65)), RangeError);
  assert.throws(() => npc.playAnimation('GetMash', { tags: 3 }), TypeError);
  assert.throws(() => npc.playAnimation('GetMash', { tags: 'a|b' }), RangeError);
  assert.throws(() => npc.playAnimation('GetMash', { tags: 'x'.repeat(129) }), RangeError);
  assert.equal(sent('npcAnimation').length, 3);
});

test('playAnimation refuses a request it cannot send', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  // Nothing to play: an empty fragment without a clip, or a fragment's id.
  assert.equal(npc.playAnimation(''), false);
  assert.equal(npc.playAnimation(7), false);
  assert.throws(() => npc.playAnimation({}, { clip: 'a' }), TypeError);
  assert.throws(() => npc.playAnimation('', null), TypeError);
  assert.throws(() => npc.playAnimation('', { clip: 'a', loop: 1 }), TypeError);
  assert.throws(() => npc.playAnimation('', { clip: 'a|b' }), RangeError);
  assert.throws(() => npc.playAnimation('', { clip: 'a\nb' }), RangeError);
  assert.throws(() => npc.playAnimation('', { clip: 'x'.repeat(257) }), RangeError);
  assert.equal(npc.playAnimation('', { clip: 'x'.repeat(256) }), true);
  assert.equal(sent('npcAnimation').length, 1);
});

test('say sends the text with its duration and empties it when the time is up', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const said = () => sent('npcSay').map((message) => [message.id, message.speech]);
  assert.equal(npc.say('Dobrý deň!', { duration: 3000 }), true);
  t.mock.timers.tick(2999);
  assert.deepEqual(said(), [[npc.id, '1|3000|Dobrý deň!']]);
  t.mock.timers.tick(1);
  assert.deepEqual(said().at(-1), [npc.id, '']);
  // By default 2 s and 60 ms a character, at most 10 s; a new line replaces the old one's end.
  npc.say('Hej');
  npc.say('x'.repeat(400));
  assert.deepEqual(said().slice(2), [
    [npc.id, '2|2180|Hej'],
    [npc.id, `3|10000|${'x'.repeat(400)}`],
  ]);
  t.mock.timers.tick(2180);
  assert.equal(said().length, 4);
  t.mock.timers.tick(7820);
  assert.deepEqual(said().at(-1), [npc.id, '']);
  // "" takes the bubble away at once.
  npc.say('Ešte jedno', { duration: 5000 });
  npc.say('');
  t.mock.timers.tick(5000);
  assert.deepEqual(said().slice(-2), [
    [npc.id, '4|5000|Ešte jedno'],
    [npc.id, ''],
  ]);
});

test('say refuses what it cannot show', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.throws(() => npc.say(5), TypeError);
  assert.throws(() => npc.say('a', null), TypeError);
  assert.throws(() => npc.say('two\nlines'), RangeError);
  assert.throws(() => npc.say('č'.repeat(201)), RangeError);
  assert.throws(() => npc.say('a', { duration: 499 }), RangeError);
  assert.throws(() => npc.say('a', { duration: 60001 }), RangeError);
  assert.throws(() => npc.say('a', { duration: 1.5 }), RangeError);
  assert.equal(sent('npcSay').length, 0);
  npc.remove();
  assert.equal(npc.say('a'), false);
});

test('setAppearance sends the four parts and keeps what a request leaves out', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const looks = () => sent('npcAppearance').map((message) => [message.id, message.appearance]);
  assert.deepEqual(npc.appearance, { body: '', head: '', hair: '', beard: '' });
  assert.equal(npc.setAppearance({ head: 'henry', beard: 'm_beard_04' }), true);
  assert.equal(npc.setAppearance({ hair: 'm_hair_001_blonde' }), true);
  assert.equal(npc.setAppearance({ beard: '' }), true);
  assert.deepEqual(looks(), [
    [npc.id, '|henry||m_beard_04'],
    [npc.id, '|henry|m_hair_001_blonde|m_beard_04'],
    [npc.id, '|henry|m_hair_001_blonde|'],
  ]);
  assert.deepEqual(npc.appearance, {
    body: '',
    head: 'henry',
    hair: 'm_hair_001_blonde',
    beard: '',
  });
});

test('setAppearance refuses names the catalog does not have for the NPC', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.setAppearance({ head: 'no_such_head' }), false);
  // A female face on a male NPC, and a beard not modelled for the face.
  assert.equal(npc.setAppearance({ head: 'f_head_000' }), false);
  assert.equal(npc.setAppearance({ head: 'henry', beard: 'm_beard_01' }), false);
  assert.throws(() => npc.setAppearance({ gender: 'female' }), TypeError);
  assert.throws(() => npc.setAppearance({ face: 'henry' }), TypeError);
  assert.throws(() => npc.setAppearance({ head: 5 }), TypeError);
  assert.throws(() => npc.setAppearance(null), TypeError);
  assert.equal(sent('npcAppearance').length, 0);
  // Create takes the same, and refuses a bad name at once.
  const dressed = Npc.create({
    position: { x: 0, y: 0, z: 0 },
    appearance: { head: 'm_head_004' },
  });
  assert.deepEqual(sent('npcAppearance').at(-1), {
    op: 'npcAppearance',
    id: dressed.id,
    appearance: '|m_head_004||',
  });
  assert.throws(
    () => Npc.create({ position: { x: 0, y: 0, z: 0 }, appearance: { head: 'x' } }),
    RangeError,
  );
  npc.remove();
  assert.equal(npc.setAppearance({ head: 'henry' }), false);
});

test('setOutfit sends the classes worn and held, and refuses what is neither', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.outfit, null);
  assert.equal(npc.setOutfit(['bow_a', { item: 'bow_b', quality: 2, health: 0.5 }]), true);
  const [message] = sent('npcOutfit');
  assert.equal(message.id, npc.id);
  assert.equal(message.pieces.length, 2);
  assert.match(message.pieces[0].itemClass, /^[0-9a-f]{32}$/);
  assert.deepEqual(
    message.pieces.map((piece) => [piece.quality, piece.health]),
    [
      [4, 1],
      [2, 0.5],
    ],
  );
  assert.deepEqual(
    npc.outfit.map((piece) => piece.quality),
    [4, 2],
  );
  // A string names a game outfit preset, which a class is not.
  assert.equal(npc.setOutfit('bow_a'), false);
  assert.throws(() => npc.setOutfit(7), TypeError);
  assert.throws(() => npc.setOutfit(['no_such_item']), RangeError);
  assert.throws(() => npc.setOutfit(['apple']), RangeError);
  assert.throws(() => npc.setOutfit([{ item: 'bow_a', quality: 9 }]), RangeError);
  assert.throws(() => npc.setOutfit([{ item: 'bow_a', health: 2 }]), RangeError);
  assert.throws(() => npc.setOutfit(new Array(33).fill('bow_a')), RangeError);
  assert.equal(sent('npcOutfit').length, 1);
  npc.remove();
  assert.equal(npc.setOutfit([]), false);
});

test('setOutfit with a preset dresses the NPC in the game outfit of its gender', async () => {
  const { Npc, sent } = await startedHost();
  const male = outfitNames('male');
  const femaleOnly = outfitNames('female').find((name) => !male.includes(name));
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const preset = lookupOutfit(male[0]);
  assert.equal(npc.setOutfit(male[0]), true);
  assert.deepEqual(
    npc.wearing,
    preset.items.map((itemClass) => itemClass.id),
  );
  const [message] = sent('npcOutfit');
  assert.equal(message.pieces.length, preset.items.length);
  // The GUID names the preset as well; the other gender's, and unknown ones, are refused.
  assert.equal(npc.setOutfit(preset.id), true);
  assert.equal(npc.setOutfit(femaleOnly), false);
  assert.equal(npc.setOutfit('no such outfit'), false);
  assert.equal(sent('npcOutfit').length, 2);
  const woman = Npc.create({ class: 'NPC_Female', position: { x: 0, y: 0, z: 0 } });
  assert.equal(woman.setOutfit(femaleOnly), true);
  assert.equal(woman.setOutfit(male.find((name) => !outfitNames('female').includes(name))), false);
});

test('wear dresses the NPC in exactly these classes and wearing lists them', async () => {
  const { Npc, sent, logs } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const bow = lookupItemClass('bow_a').id;
  const arrow = lookupItemClass('arrow_crude').id;
  const apple = lookupItemClass('apple').id;
  assert.deepEqual(npc.wearing, []);
  assert.equal(npc.wear([bow, arrow, arrow]), true);
  assert.deepEqual(npc.wearing, [bow, arrow, arrow]);
  const [message] = sent('npcOutfit');
  assert.equal(message.pieces.length, 3);
  assert.deepEqual(
    npc.outfit.map((piece) => [piece.item, piece.quality, piece.health]),
    [
      [bow, 4, 1],
      [arrow, lookupItemClass('arrow_crude').maximumQuality, 1],
      [arrow, lookupItemClass('arrow_crude').maximumQuality, 1],
    ],
  );
  // It replaces what the NPC had on; setOutfit's items are listed in wearing as well.
  assert.equal(npc.wear([arrow]), true);
  assert.deepEqual(npc.wearing, [arrow]);
  assert.equal(npc.setOutfit([bow]), true);
  assert.deepEqual(npc.wearing, [bow]);
  // Names are taken as well as GUIDs.
  assert.equal(npc.wear(['bow_a']), true);
  assert.deepEqual(npc.wearing, [bow]);
  assert.equal(sent('npcOutfit').length, 4);
  // One entry that is no class worn or held, and nothing changes: each such entry is logged.
  const nobody = '00000000-0000-0000-0000-000000000000';
  assert.equal(npc.wear([arrow, nobody, apple]), false);
  assert.deepEqual(npc.wearing, [bow]);
  assert.equal(sent('npcOutfit').length, 4);
  assert.equal(logs.filter((text) => text.includes(nobody)).length, 1);
  assert.equal(logs.filter((text) => text.includes(apple)).length, 1);
  assert.equal(npc.wear([7]), false);
  assert.throws(() => npc.wear(bow), TypeError);
  assert.throws(() => npc.wear(new Array(33).fill(bow)), RangeError);
  // An empty list undresses the NPC.
  assert.equal(npc.wear([]), true);
  assert.deepEqual(npc.wearing, []);
  assert.equal(sent('npcOutfit').at(-1).pieces.length, 0);
  npc.remove();
  assert.equal(npc.wear([bow]), false);
});

test('create dresses the NPC by outfit or wearing and refuses what it cannot wear', async () => {
  const { Npc, sent } = await startedHost();
  const at = { position: { x: 0, y: 0, z: 0 } };
  const bow = lookupItemClass('bow_a').id;
  const preset = outfitNames('male')[0];
  const dressed = Npc.create({ ...at, wearing: [bow] });
  assert.deepEqual(dressed.wearing, [bow]);
  const [first] = sent('npcOutfit');
  assert.equal(first.id, dressed.id);
  assert.equal(first.pieces.length, 1);
  assert.ok(sent('npcSpawn').findIndex((spawn) => spawn.id === dressed.id) >= 0);
  const clothed = Npc.create({ ...at, outfit: preset });
  assert.deepEqual(
    clothed.wearing,
    lookupOutfit(preset).items.map((itemClass) => itemClass.id),
  );
  // Both given: the list is worn in place of the preset.
  const both = Npc.create({ ...at, outfit: preset, wearing: [bow] });
  assert.deepEqual(both.wearing, [bow]);
  const plain = Npc.create(at);
  assert.deepEqual(plain.wearing, []);
  const spawned = sent('npcSpawn').length;
  assert.throws(() => Npc.create({ ...at, outfit: 'no such outfit' }), RangeError);
  assert.throws(() => Npc.create({ ...at, outfit: 5 }), TypeError);
  assert.throws(() => Npc.create({ ...at, wearing: ['no_such_item'] }), RangeError);
  assert.throws(() => Npc.create({ ...at, wearing: bow }), TypeError);
  assert.equal(sent('npcSpawn').length, spawned, 'a refused outfit spawns nothing');
});

test('create applies health, invulnerable, lootable and appearance and their defaults', async () => {
  const { Npc, sent } = await startedHost();
  const at = { position: { x: 0, y: 0, z: 0 } };
  const plain = Npc.create(at);
  assert.equal(plain.health, 100);
  assert.equal(plain.maxHealth, 100);
  assert.equal(plain.invulnerable, false);
  assert.equal(plain.lootable, true);
  assert.equal(plain.frozen, false);
  assert.equal(plain.alive, true);
  assert.equal(sent('npcHealth').length, 0);
  const set = Npc.create({ ...at, health: 40, invulnerable: true, lootable: false });
  assert.equal(set.health, 40);
  assert.equal(set.invulnerable, true);
  assert.equal(set.lootable, false);
  assert.equal(sent('npcHealth').length, 1);
  const styled = Npc.create({ ...at, appearance: { head: 'henry' } });
  assert.equal(styled.appearance.head, 'henry');
  assert.equal(sent('npcAppearance').length, 1);
  // A health above the maximum is clamped to it, as the ledger is.
  assert.equal(Npc.create({ ...at, health: 500 }).health, 100);
});

test('a hit takes health, raises npcDamage and at 0 npcDeath with the attacker', async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const player = host.updatePlayer({
    id: 1,
    nickname: 'P1',
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const events = [];
  host.on('npcDamage', (who, attacker, amount, bodyPart, reason) => {
    events.push(['damage', who, attacker, amount, bodyPart, reason]);
  });
  host.on('npcDeath', (who, killer) => events.push(['death', who, killer]));
  host.npcs.hit({ id: npc.id, attackerId: 1, amount: 30, reason: 1, subpart: 23 });
  assert.equal(npc.health, 70);
  host.npcs.hit({ id: npc.id, attackerId: 0, amount: 90, reason: 7, subpart: -1 });
  assert.equal(npc.health, 0);
  assert.equal(npc.alive, false);
  // A dead NPC takes no more.
  host.npcs.hit({ id: npc.id, attackerId: 1, amount: 5, reason: 1, subpart: -1 });
  // npcDamage says what the ledger lost: the 90 of the fall took the 70 that were left.
  assert.deepEqual(events, [
    ['damage', npc, player, 30, 'head', 'combat'],
    ['damage', npc, null, 70, null, 'fall'],
    ['death', npc, null],
  ]);
  assert.deepEqual(
    sent('npcHealth').map((message) => [message.id, message.health, message.alive]),
    [
      [npc.id, 70, true],
      [npc.id, 0, false],
    ],
  );
});

test('npcHit decides a hit before the NPC loses health: refuse it or set its damage', async () => {
  const { host, Npc } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  host.updatePlayer({
    id: 1,
    nickname: 'P1',
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const seen = [];
  host.on('npcHit', (who, attacker, hit) => {
    seen.push([who, attacker?.id ?? null, hit.damage, hit.bodyPart, hit.reason]);
    if (hit.reason === 'fall') {
      return false;
    }
    if (hit.bodyPart === 'head') {
      hit.damage = 50;
    }
    if (hit.bodyPart === 'torso') {
      hit.damage = 0;
    }
  });
  const damaged = [];
  host.on('npcDamage', (who, attacker, amount) => damaged.push(amount));
  host.npcs.hit({ id: npc.id, attackerId: 1, amount: 30, reason: 1, subpart: 23 });
  assert.equal(npc.health, 50);
  host.npcs.hit({ id: npc.id, attackerId: 0, amount: 90, reason: 7, subpart: -1 });
  assert.equal(npc.health, 50);
  host.npcs.hit({ id: npc.id, attackerId: 1, amount: 10, reason: 1, subpart: 2 });
  assert.equal(npc.health, 50);
  host.npcs.hit({ id: npc.id, attackerId: 1, amount: 10, reason: 1, subpart: -1 });
  assert.equal(npc.health, 40);
  assert.deepEqual(damaged, [50, 10]);
  assert.deepEqual(
    seen.map(([who, attacker, damage, bodyPart, reason]) => [
      who === npc,
      attacker,
      damage,
      bodyPart,
      reason,
    ]),
    [
      [true, 1, 30, 'head', 'combat'],
      [true, null, 90, null, 'fall'],
      [true, 1, 10, 'torso', 'combat'],
      [true, 1, 10, null, 'combat'],
    ],
  );
});

test('damage, kill, revive and the health setter send the health and raise the events', async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 }, health: 50 });
  const other = Npc.create({ position: { x: 1, y: 0, z: 0 } });
  const events = [];
  host.on('npcDamage', (who, attacker, amount, bodyPart, reason) => {
    events.push(['damage', attacker, amount, reason]);
  });
  host.on('npcDeath', (who, killer) => events.push(['death', killer]));
  host.on('npcRevive', (who) => events.push(['revive', who]));
  assert.equal(npc.health, 50);
  assert.equal(npc.damage(20, other), true);
  npc.health = 80;
  assert.equal(npc.kill(other), true);
  assert.equal(npc.kill(), false);
  assert.equal(npc.damage(5), false);
  // Setting health does not bring a dead NPC back.
  npc.health = 40;
  assert.equal(npc.health, 0);
  assert.equal(npc.revive(), true);
  assert.equal(npc.revive(), false);
  assert.equal(npc.health, 100);
  npc.health = 0;
  assert.equal(npc.alive, false);
  assert.equal(npc.revive(25), true);
  assert.deepEqual(events, [
    ['damage', other, 20, 'scripted'],
    ['death', other],
    ['revive', npc],
    ['death', null],
    ['revive', npc],
  ]);
  assert.deepEqual(
    sent('npcHealth')
      .filter((message) => message.id === npc.id)
      .map((message) => [message.health, message.alive]),
    [
      [50, true],
      [30, true],
      [80, true],
      [0, false],
      [100, true],
      [0, false],
      [25, true],
    ],
  );
});

test('damage and kill take a Player, a player id, an Npc or nobody as the attacker', async () => {
  const { host, Npc } = await startedHost();
  const player = host.updatePlayer({
    id: 1,
    nickname: 'P1',
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const other = Npc.create({ position: { x: 1, y: 0, z: 0 } });
  const events = [];
  host.on('npcDamage', (who, attacker, amount) => events.push(['damage', who, attacker, amount]));
  host.on('npcDeath', (who, killer) => events.push(['death', who, killer]));
  npc.damage(10, player);
  // A number is a player's id, resolved to the Player; an id nobody has is no attacker.
  npc.damage(10, 1);
  npc.damage(10, 99);
  npc.damage(10, other);
  npc.damage(10);
  assert.equal(npc.health, 50);
  npc.kill(1);
  assert.equal(npc.alive, false);
  npc.revive();
  npc.kill(99);
  npc.revive();
  npc.kill(other);
  npc.revive();
  npc.kill();
  assert.deepEqual(events, [
    ['damage', npc, player, 10],
    ['damage', npc, player, 10],
    ['damage', npc, null, 10],
    ['damage', npc, other, 10],
    ['damage', npc, null, 10],
    ['death', npc, player],
    ['death', npc, null],
    ['death', npc, other],
    ['death', npc, null],
  ]);
  // The arguments of the events: the NPC first, then who and how much.
  const arguments_ = [];
  host.on('npcRevive', (...given) => arguments_.push(given));
  npc.revive();
  assert.deepEqual(arguments_, [[npc]]);
  assert.throws(() => npc.damage(5, 'x'), TypeError);
  assert.throws(() => npc.damage(5, {}), TypeError);
  assert.throws(() => npc.kill(true), TypeError);
});

test('the health setter and create clamp a health above the maximum', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 }, health: 50 });
  npc.health = 500;
  assert.equal(npc.health, 100);
  npc.health = Infinity;
  assert.equal(npc.health, 100);
  assert.deepEqual(
    sent('npcHealth').map((message) => message.health),
    [50, 100, 100],
  );
  assert.throws(() => {
    npc.health = NaN;
  }, RangeError);
});

test('an invulnerable NPC takes no hits or damage but can be killed', async () => {
  const { host, Npc } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 }, invulnerable: true });
  host.npcs.hit({ id: npc.id, attackerId: 0, amount: 30, reason: 1, subpart: -1 });
  assert.equal(npc.damage(30), false);
  assert.equal(npc.health, 100);
  npc.invulnerable = false;
  assert.equal(npc.damage(30), true);
  assert.equal(npc.health, 70);
  npc.invulnerable = true;
  assert.equal(npc.kill(), true);
  assert.equal(npc.alive, false);
});

test('the health members refuse what they cannot use', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.throws(() => Npc.create({ position: { x: 0, y: 0, z: 0 }, health: 0 }), RangeError);
  assert.throws(() => Npc.create({ position: { x: 0, y: 0, z: 0 }, health: '50' }), RangeError);
  assert.throws(() => Npc.create({ position: { x: 0, y: 0, z: 0 }, invulnerable: 1 }), TypeError);
  assert.throws(() => {
    npc.health = -1;
  }, RangeError);
  assert.throws(() => {
    npc.health = '50';
  }, RangeError);
  assert.throws(() => {
    npc.invulnerable = 'yes';
  }, TypeError);
  assert.throws(() => npc.damage(0), RangeError);
  assert.throws(() => npc.damage(Infinity), RangeError);
  assert.throws(() => npc.damage(5, { id: 1 }), TypeError);
  assert.throws(() => npc.kill('x'), TypeError);
  npc.kill();
  assert.throws(() => npc.revive(0), RangeError);
  assert.throws(() => npc.revive(101), RangeError);
  assert.equal(sent('npcHealth').length, 1);
  npc.remove();
  assert.equal(npc.revive(), false);
  assert.equal(npc.damage(5), false);
  assert.equal(npc.kill(), false);
});

test('a removed NPC sends nothing more and is gone from the lookups', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  npc.remove();
  assert.deepEqual(
    sent('npcDestroy').map((message) => message.id),
    [npc.id],
  );
  assert.equal(npc.playAnimation('', { clip: 'a' }), false);
  assert.equal(npc.stopAnimation(), false);
  assert.equal(sent('npcAnimation').length, 0);
  assert.equal(Npc.getById(npc.id), null);
  assert.deepEqual(Npc.all(), []);
});

test('reports move the NPC and a new simulator raises npcSimulatorChange', async () => {
  const { host, Npc } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const player = host.updatePlayer({
    id: 1,
    nickname: 'P1',
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const changes = [];
  host.on('npcSimulatorChange', (who, simulator, previous) => {
    changes.push([who, simulator, previous]);
  });
  host.npcs.update({
    id: npc.id,
    position: { x: 5, y: 6, z: 7 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.deepEqual([npc.position.x, npc.position.y, npc.position.z], [5, 6, 7]);
  host.npcs.simulatorChanged({ id: npc.id, simulatorId: 1 });
  host.npcs.simulatorChanged({ id: npc.id, simulatorId: 1 });
  host.npcs.simulatorChanged({ id: npc.id, simulatorId: 0 });
  assert.deepEqual(changes, [
    [npc, player, null],
    [npc, null, player],
  ]);
});

// The orders: each goes to the native server as the NPC's intent text.

// A player the host knows, with `health` in the vitals its owner reported (`alive` follows it).
function nearbyPlayer(host, id, position, health = 50) {
  const vitals = {
    health,
    stamina: 100,
    exhaust: 100,
    hunger: 100,
    bleeding: 0,
    sleeping: 0,
    consciousness: 1,
    drunkenness: 0,
    poisoning: 0,
  };
  return host.updatePlayer({
    id,
    nickname: `P${id}`,
    ready: true,
    position,
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    vitals,
  });
}

test('moveTo sends the destination with its speed, radius and pathfinding', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 1, y: 2, z: 3 } });
  const intents = () => sent('npcIntent').map((message) => [message.id, message.intent]);
  assert.equal(npc.intent, 'hold');
  assert.equal(npc.status, 'idle');
  assert.equal(npc.moveTo({ x: 10, y: 20, z: 30 }), true);
  assert.equal(npc.intent, 'moveTo');
  assert.equal(npc.status, 'running');
  assert.equal(
    npc.moveTo({ x: 10.126, z: 31 }, { speed: 'run', radius: 2.5, pathfinding: 'game' }),
    true,
  );
  assert.deepEqual(intents(), [
    [npc.id, '1|moveTo|10|20|30|walk|1.5|auto'],
    [npc.id, '2|moveTo|10.13|2|31|run|2.5|game'],
  ]);
  // A destination it cannot use, or an NPC that is dead, sends nothing and keeps the order.
  assert.equal(npc.moveTo({ x: 0, y: 0, z: 20000 }), false);
  assert.equal(npc.moveTo({ x: Number.NaN, y: 0, z: 0 }), false);
  assert.equal(npc.moveTo(null), false);
  npc.kill();
  assert.equal(npc.moveTo({ x: 1, y: 1, z: 1 }), false);
  assert.equal(sent('npcIntent').length, 2);
  assert.throws(() => npc.moveTo({ x: 1, y: 1, z: 1 }, null), TypeError);
  assert.throws(() => npc.moveTo({ x: 1, y: 1, z: 1 }, { speed: 5 }), TypeError);
  assert.throws(() => npc.moveTo({ x: 1, y: 1, z: 1 }, { speed: 'sprint' }), RangeError);
  assert.throws(() => npc.moveTo({ x: 1, y: 1, z: 1 }, { radius: '2' }), TypeError);
  assert.throws(() => npc.moveTo({ x: 1, y: 1, z: 1 }, { radius: 0 }), RangeError);
  assert.throws(() => npc.moveTo({ x: 1, y: 1, z: 1 }, { pathfinding: 'maybe' }), RangeError);
  npc.remove();
  assert.equal(npc.moveTo({ x: 1, y: 1, z: 1 }), false);
});

test('patrol orders its first leg as a move and shows the patrol', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 1, y: 2, z: 3 } });
  const route = [
    { x: 10, y: 20, z: 30 },
    { x: 11, y: 21 },
  ];
  assert.equal(npc.routeState, null);
  assert.equal(npc.patrol(route), true);
  assert.equal(npc.intent, 'moveTo');
  assert.equal(npc.status, 'running');
  assert.deepEqual(npc.routeState, {
    routeId: null,
    waypointIndex: 0,
    lap: 0,
    direction: 1,
    phase: 'moving',
  });
  assert.equal(
    npc.patrol(route, { speed: 'jog', mode: 'pingPong', radius: 2, waitSeconds: 5.5 }),
    true,
  );
  assert.equal(npc.patrol(route, { loop: false, pathfinding: 'server' }), true);
  assert.deepEqual(
    sent('npcIntent').map((message) => message.intent),
    [
      '1|moveTo|10|20|30|walk|1.5|auto',
      '2|moveTo|10|20|30|jog|2|auto',
      '3|moveTo|10|20|30|walk|1.5|server',
    ],
  );
  // The patrol state is a copy; any other order ends the patrol.
  npc.routeState.lap = 9;
  assert.equal(npc.routeState.lap, 0);
  assert.equal(npc.moveTo({ x: 5, y: 5, z: 5 }), true);
  assert.equal(npc.routeState, null);
  // What answers with false.
  assert.equal(npc.patrol([]), false);
  assert.equal(npc.patrol(new Array(257).fill({ x: 1, y: 1, z: 1 })), false);
  assert.equal(npc.patrol(new Array(256).fill({ x: 1, y: 1, z: 1 })), true);
  const outside = [
    { x: 1, y: 1, z: 1 },
    { x: -5, y: 1, z: 1 },
  ];
  assert.equal(npc.patrol(outside), false);
  assert.equal(npc.patrol([{ x: 1, y: 1, z: 1 }, null]), false);
  assert.equal(npc.patrol(route, { waitSeconds: '5' }), false);
  assert.equal(npc.patrol(route, { waitSeconds: Number.NaN }), false);
  assert.equal(sent('npcIntent').length, 5);
  assert.equal(npc.patrol('town.guard-loop'), false, 'no such route');
  assert.throws(() => npc.patrol(5), TypeError);
  assert.throws(() => npc.patrol(null), TypeError);
  assert.throws(() => npc.patrol(route, { loop: true, mode: 'once' }), TypeError);
  assert.throws(() => npc.patrol(route, { loop: 1 }), TypeError);
  assert.throws(() => npc.patrol(route, { mode: 'twice' }), RangeError);
  assert.throws(() => npc.patrol(route, { waitSeconds: 3601 }), RangeError);
  assert.throws(() => npc.patrol(route, { waitSeconds: -1 }), RangeError);
  assert.throws(() => npc.patrol(route, { speed: 'sprint' }), RangeError);
});

test('follow sends the target by its id, a Player or an Npc', async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const other = Npc.create({ position: { x: 5, y: 5, z: 5 } });
  const player = nearbyPlayer(host, 1, { x: 3, y: 3, z: 3 });
  assert.equal(npc.follow(player), true);
  assert.equal(npc.follow(other, { speed: 'jog', radius: 4.5 }), true);
  assert.equal(npc.follow(1, { radius: 2 }), true);
  assert.equal(npc.intent, 'follow');
  assert.equal(npc.status, 'running');
  assert.deepEqual(
    sent('npcIntent').map((message) => message.intent),
    ['1|follow|1|walk|3', `2|follow|${other.id}|jog|4.5`, '3|follow|1|walk|2'],
  );
  // A target that is gone, or the NPC itself, is no order; a wrong type or option throws.
  assert.equal(npc.follow(999), false);
  assert.equal(npc.follow(npc), false);
  other.remove();
  assert.equal(npc.follow(other), false);
  assert.equal(sent('npcIntent').length, 3);
  assert.throws(() => npc.follow('1'), TypeError);
  assert.throws(() => npc.follow({}), TypeError);
  assert.throws(() => npc.follow(1.5), TypeError);
  assert.throws(() => npc.follow(player, { radius: -1 }), RangeError);
  assert.throws(() => npc.follow(player, { speed: 'fly' }), RangeError);
});

test('flee sends the place to run from, running by default', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 1, y: 2, z: 3 } });
  assert.equal(npc.flee({ x: 7, y: 8, z: 9 }), true);
  assert.equal(npc.flee({ x: 7 }, { speed: 'walk', radius: 12 }), true);
  assert.equal(npc.intent, 'flee');
  assert.deepEqual(
    sent('npcIntent').map((message) => message.intent),
    ['1|flee|7|8|9|run|30', '2|flee|7|2|3|walk|12'],
  );
  assert.equal(npc.flee({ x: 7, y: 8, z: Number.POSITIVE_INFINITY }), false);
  assert.equal(npc.flee('here'), false);
  assert.equal(sent('npcIntent').length, 2);
  assert.throws(() => npc.flee({ x: 7, y: 8, z: 9 }, { radius: 0 }), RangeError);
  assert.throws(() => npc.flee({ x: 7, y: 8, z: 9 }, { speed: 1 }), TypeError);
});

test('lookAt sends an entity or a point and has no end to report', async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 1, y: 2, z: 3 } });
  const player = nearbyPlayer(host, 1, { x: 3, y: 3, z: 3 });
  npc.moveTo({ x: 9, y: 9, z: 9 });
  assert.equal(npc.lookAt(player), true);
  assert.equal(npc.intent, 'lookAt');
  assert.equal(npc.status, 'idle');
  assert.equal(npc.lookAt(1), true);
  assert.equal(npc.lookAt({ x: 4, y: 5.5, z: 6 }), true);
  const looks = sent('npcIntent').map((message) => message.intent);
  assert.deepEqual(looks.slice(1), [
    '2|lookAt|entity|1',
    '3|lookAt|entity|1',
    '4|lookAt|point|4|5.5|6',
  ]);
  assert.equal(npc.lookAt(999), false);
  assert.equal(npc.lookAt(npc), false);
  assert.equal(npc.lookAt({ x: 4, y: 5, z: -2000 }), false);
  assert.equal(npc.lookAt('player'), false);
  assert.equal(sent('npcIntent').length, 4);
});

test('attack sends the target and weapon for a live body within reach', async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 }, dimension: 1 });
  const near = Npc.create({ position: { x: 10, y: 0, z: 0 }, dimension: 1 });
  const far = Npc.create({ position: { x: 129, y: 0, z: 0 }, dimension: 1 });
  const elsewhere = Npc.create({ position: { x: 10, y: 0, z: 0 }, dimension: 2 });
  const everywhere = Npc.create({ position: { x: 10, y: 0, z: 0 } });
  const player = nearbyPlayer(host, 1, { x: 20, y: 0, z: 0 });
  assert.equal(npc.attack(near), true);
  assert.equal(npc.attack(player, { weapon: 'bow' }), true);
  assert.equal(npc.attack(everywhere.id, { weapon: 'melee' }), true);
  assert.equal(npc.intent, 'attack');
  assert.equal(npc.status, 'running');
  assert.deepEqual(
    sent('npcIntent').map((message) => message.intent),
    [`1|attack|${near.id}|melee`, '2|attack|1|bow', `3|attack|${everywhere.id}|melee`],
  );
  // Out of reach, in another world, dead, gone or itself: false, and the order stays.
  assert.equal(npc.attack(far), false);
  assert.equal(npc.attack(elsewhere), false);
  assert.equal(npc.attack(npc), false);
  assert.equal(npc.attack(999), false);
  near.kill();
  assert.equal(npc.attack(near), false);
  nearbyPlayer(host, 2, { x: 5, y: 0, z: 0 }, 0);
  assert.equal(npc.attack(2), false);
  assert.equal(sent('npcIntent').length, 3);
  assert.equal(npc.intent, 'attack');
  assert.throws(() => npc.attack(player, { weapon: 'axe' }), RangeError);
  assert.throws(() => npc.attack(player, { weapon: 3 }), TypeError);
  assert.throws(() => npc.attack(player, null), TypeError);
  assert.throws(() => npc.attack('1'), TypeError);
  assert.throws(() => npc.attack(), TypeError);
});

test('a blow in a fight makes a person attack the striker, unless told otherwise', async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const other = Npc.create({ position: { x: 3, y: 0, z: 0 } });
  const boar = Npc.create({ class: 'Boar', position: { x: 0, y: 3, z: 0 } });
  const calm = Npc.create({ position: { x: 3, y: 3, z: 0 }, fightsBack: false });
  nearbyPlayer(host, 1, { x: 1, y: 0, z: 0 });
  nearbyPlayer(host, 2, { x: 2, y: 0, z: 0 });
  assert.deepEqual(
    [npc.fightsBack, boar.fightsBack, calm.fightsBack],
    [true, false, false],
    'people fight back by default, animals do not',
  );
  const intents = () => sent('npcIntent').map((message) => [message.id, message.intent]);
  host.npcs.hit({ id: npc.id, attackerId: 1, amount: 10, reason: 1, subpart: -1 });
  assert.deepEqual(intents(), [[npc.id, '1|attack|1|melee']]);
  // Attacking already: another blow, even by someone else, keeps the fight it is in.
  host.npcs.hit({ id: npc.id, attackerId: 2, amount: 10, reason: 1, subpart: -1 });
  assert.equal(intents().length, 1);
  // Once that fight ended, the next striker is attacked; a blow by an NPC as well.
  host.npcs.intentDone({ id: npc.id, status: 'failed', sequence: 1 });
  host.npcs.hit({ id: npc.id, attackerId: 2, amount: 10, reason: 1, subpart: -1 });
  other.attack(npc);
  host.npcs.intentDone({ id: npc.id, status: 'failed', sequence: 2 });
  host.npcs.hit({ id: npc.id, attackerId: other.id, amount: 5, reason: 1, subpart: -1 });
  // A fall, a scripted blow, nobody, an animal and an NPC told not to: no order.
  host.npcs.hit({ id: npc.id, attackerId: 0, amount: 5, reason: 7, subpart: -1 });
  npc.damage(5, 1);
  host.npcs.hit({ id: boar.id, attackerId: 1, amount: 5, reason: 1, subpart: -1 });
  host.npcs.hit({ id: calm.id, attackerId: 1, amount: 5, reason: 1, subpart: -1 });
  // An npcDamage handler's own order stands.
  calm.fightsBack = true;
  host.on('npcDamage', (who) => {
    if (who === calm) {
      who.flee({ x: 0, y: 0, z: 0 });
    }
  });
  host.npcs.hit({ id: calm.id, attackerId: 1, amount: 5, reason: 1, subpart: -1 });
  assert.deepEqual(intents().slice(1), [
    [npc.id, '2|attack|2|melee'],
    [other.id, `1|attack|${npc.id}|melee`],
    [npc.id, `3|attack|${other.id}|melee`],
    [calm.id, intents()[4][1]],
  ]);
  assert.match(intents()[4][1], /^1\|flee\|/);
  // A blow that kills sends the dead NPC no order; the NPC that attacked it stops.
  host.npcs.intentDone({ id: npc.id, status: 'failed', sequence: 3 });
  host.npcs.hit({ id: npc.id, attackerId: 1, amount: 100, reason: 1, subpart: -1 });
  assert.deepEqual(intents().slice(5), [[other.id, '']]);
  assert.throws(() => {
    npc.fightsBack = 1;
  }, TypeError);
  assert.throws(() => Npc.create({ position: { x: 0, y: 0, z: 0 }, fightsBack: 'yes' }), TypeError);
});

test('a target knocked out: the NPC turns to whoever still fights it, else stops', async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const first = nearbyPlayer(host, 1, { x: 1, y: 0, z: 0 });
  nearbyPlayer(host, 2, { x: 2, y: 0, z: 0 });
  const intents = () => sent('npcIntent').map((message) => message.intent);
  // Player 1 strikes: the NPC attacks them; player 2 strikes too while it does.
  host.npcs.hit({ id: npc.id, attackerId: 1, amount: 5, reason: 1, subpart: -1 });
  host.npcs.hit({ id: npc.id, attackerId: 2, amount: 5, reason: 1, subpart: -1 });
  assert.deepEqual(intents(), ['1|attack|1|melee']);
  // Player 1 goes down: player 2 still fights, so the NPC turns to them.
  host.playerDied(1);
  assert.equal(first.knockedOut, true);
  assert.deepEqual(intents().at(-1), '2|attack|2|melee');
  // Player 2 goes down too: nobody fights it any more, it stops (not frozen).
  host.playerDied(2);
  assert.equal(intents().at(-1), '');
  assert.equal(npc.intent, 'hold');
  assert.equal(npc.frozen, false);
});

test('attack refuses a knocked-out player: an NPC does not finish them off', async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const player = nearbyPlayer(host, 1, { x: 1, y: 0, z: 0 });
  host.playerDied(1);
  assert.equal(player.knockedOut, true);
  assert.equal(npc.attack(player), false);
  assert.equal(sent('npcIntent').length, 0);
  // Revived, they are a target again.
  player.revive();
  assert.equal(npc.attack(player), true);
  assert.deepEqual(
    sent('npcIntent').map((message) => message.intent),
    ['1|attack|1|melee'],
  );
});

test('setCombatSkill sends the defence share and checks it', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.deepEqual(npc.combatSkill, { defense: null, damage: null });
  assert.equal(npc.setCombatSkill({ defense: 0.5 }), true);
  assert.deepEqual(npc.combatSkill, { defense: 0.5, damage: null });
  assert.deepEqual(
    sent('npcDefense').map((message) => message.defense),
    [0.5],
  );
  // A value left out keeps what it has; null gives the game's own defences back.
  npc.setCombatSkill({});
  assert.equal(sent('npcDefense').length, 1);
  npc.setCombatSkill({ defense: null });
  assert.deepEqual(npc.combatSkill, { defense: null, damage: null });
  assert.equal(sent('npcDefense').at(-1).defense, -1);
  assert.throws(() => npc.setCombatSkill({ defense: 1.5 }), RangeError);
  assert.throws(() => npc.setCombatSkill({ defense: -0.1 }), RangeError);
  assert.throws(() => npc.setCombatSkill({ defense: '1' }), RangeError);
  assert.throws(() => npc.setCombatSkill(null), TypeError);
});

test('setCombatSkill sends the share of damage its blows on players take', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.setCombatSkill({ damage: 0.5 }), true);
  assert.deepEqual(npc.combatSkill, { defense: null, damage: 0.5 });
  npc.setCombatSkill({ damage: 2, defense: 0.25 });
  npc.setCombatSkill({ damage: null });
  assert.deepEqual(npc.combatSkill, { defense: 0.25, damage: null });
  assert.deepEqual(
    sent('npcDamage').map((message) => message.damage),
    [0.5, 2, 1],
  );
  assert.throws(() => npc.setCombatSkill({ damage: 11 }), RangeError);
  assert.throws(() => npc.setCombatSkill({ damage: -1 }), RangeError);
  assert.throws(() => npc.setCombatSkill({ damage: 'x' }), RangeError);
});

test('hold clears the order and freezes the body', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  npc.patrol([{ x: 1, y: 1, z: 1 }]);
  npc.hold();
  assert.equal(npc.intent, 'hold');
  assert.equal(npc.status, 'idle');
  assert.equal(npc.routeState, null);
  assert.equal(npc.frozen, true);
  assert.deepEqual(
    sent('npcIntent').map((message) => message.intent),
    ['1|moveTo|1|1|1|walk|1.5|auto', ''],
  );
  assert.equal(sent('npcFrozen').length, 1);
  // The next order is numbered after the one that was cancelled, and frees the held body.
  npc.moveTo({ x: 2, y: 2, z: 2 });
  assert.equal(sent('npcIntent').at(-1).intent.startsWith('3|moveTo|'), true);
  assert.equal(npc.frozen, false);
  // A body the resource froze itself stays frozen through a hold and the next order.
  npc.frozen = true;
  npc.hold();
  npc.moveTo({ x: 3, y: 3, z: 3 });
  assert.equal(npc.frozen, true);
  npc.remove();
  assert.throws(() => npc.hold(), /Unknown NPC/);
});

test('intentDone raises npcIntentDone once the order ended', async () => {
  const { host, Npc } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const done = [];
  host.on('npcIntentDone', (who, status) => done.push([who, status, who.status]));
  npc.moveTo({ x: 5, y: 5, z: 5 });
  // A report of an earlier order, an unknown status and an unknown NPC raise nothing.
  host.npcs.intentDone({ id: npc.id, status: 'reached', sequence: 7 });
  host.npcs.intentDone({ id: npc.id, status: 'arrived' });
  host.npcs.intentDone({ id: npc.id + 1000, status: 'reached' });
  assert.deepEqual(done, []);
  assert.equal(npc.status, 'running');
  host.npcs.intentDone({ id: npc.id, status: 'reached', sequence: 1 });
  assert.deepEqual(done, [[npc, 'reached', 'reached']]);
  assert.equal(npc.status, 'reached');
  npc.moveTo({ x: 6, y: 6, z: 6 });
  assert.equal(npc.status, 'running');
  host.npcs.intentDone({ id: npc.id, status: 'blocked' });
  assert.equal(npc.status, 'blocked');
  // A hold makes an earlier order's report stale.
  npc.moveTo({ x: 7, y: 7, z: 7 });
  npc.hold();
  host.npcs.intentDone({ id: npc.id, status: 'failed', sequence: 3 });
  assert.equal(done.length, 2);
  assert.equal(npc.status, 'idle');
});

test('the npcIntentDone op as the server sends it lands in intentDone', async () => {
  const { host, Npc } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const done = [];
  host.on('npcIntentDone', (who, status) => done.push([who, status]));
  npc.moveTo({ x: 5, y: 5, z: 5 });
  // The op of runtime.hpp report_npc_intent_dones: the order's number is the leading number of
  // the intent text (here 1), and a report of another order is stale.
  host.npcs.intentDone({ op: 'npcIntentDone', id: npc.id, status: 'failed', sequence: 9 });
  assert.deepEqual(done, []);
  host.npcs.intentDone({ op: 'npcIntentDone', id: npc.id, status: 'blocked', sequence: 1 });
  assert.deepEqual(done, [[npc, 'blocked']]);
  assert.equal(npc.status, 'blocked');
});

test("an NPC's own line from its simulator shows for everyone, marked as the game's", async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  nearbyPlayer(host, 1, { x: 1, y: 0, z: 0 });
  nearbyPlayer(host, 2, { x: 2, y: 0, z: 0 });
  host.npcs.simulatorChanged({ id: npc.id, simulatorId: 1 });
  // Another player's report is not taken.
  assert.equal(host.npcs.barked({ id: npc.id, from: 2, text: 'Hey!', localize: false }), false);
  assert.equal(host.npcs.barked({ id: npc.id, from: 1, text: 'bark_key', localize: true }), true);
  const said = sent('npcSay').at(-1);
  assert.equal(said.id, npc.id);
  const [, duration, text] = said.speech.split('|');
  assert.ok(Number(duration) >= 2500);
  assert.equal(text, '⁣⁤bark_key');
});

test("an NPC's own spoken line carries its voice file for the other players", async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  nearbyPlayer(host, 1, { x: 1, y: 0, z: 0 });
  host.npcs.simulatorChanged({ id: npc.id, simulatorId: 1 });
  const voice = 'Localization/dialog/town/bark_greeting_01';
  assert.equal(
    host.npcs.barked({ id: npc.id, from: 1, text: 'bark_key', localize: true, voice, purpose: 7 }),
    true,
  );
  const [, , text] = sent('npcSay').at(-1).speech.split('|');
  assert.equal(text, `⁣⁤⁢${voice}⁢7⁢bark_key`);
});

test('a line said without a bubble goes with its voice alone', async () => {
  const { host, Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  nearbyPlayer(host, 1, { x: 1, y: 0, z: 0 });
  host.npcs.simulatorChanged({ id: npc.id, simulatorId: 1 });
  const voice = 'Localization/dialog/town/greeting_02';
  assert.equal(host.npcs.barked({ id: npc.id, from: 1, text: '', localize: false }), false);
  assert.equal(host.npcs.barked({ id: npc.id, from: 1, text: '', localize: false, voice }), true);
  const [, , text] = sent('npcSay').at(-1).speech.split('|');
  assert.equal(text, `⁣⁢${voice}⁢0⁢`);
});

// Roles, nameplates, locomotion, faction and pin.

test('a role names one of the game souls and its body', async () => {
  const { Npc, sent } = await startedHost();
  const roles = Npc.roles();
  assert.ok(roles.includes('guard'));
  assert.ok(roles.includes('townswoman'));
  const guard = Npc.create({ soul: 'guard', position: { x: 0, y: 0, z: 0 } });
  const woman = Npc.create({ soul: 'townswoman', position: { x: 1, y: 0, z: 0 } });
  const [first, second] = sent('npcSpawn');
  assert.equal(first.soul, '63daa80b-d1f4-46f5-8222-c22a541cae07');
  assert.equal(first.actorClass, 'NPC');
  assert.equal(second.actorClass, 'NPC_Female');
  assert.equal(guard.soul, first.soul);
  assert.equal(woman.actorClass, 'NPC_Female');
});

test('the nameplate is off by default and every change goes down as the field', async () => {
  const { Npc, sent } = await startedHost();
  const quiet = Npc.create({ soul: SOUL, position: { x: 0, y: 0, z: 0 } });
  assert.equal(quiet.nameplate, false);
  assert.equal(sent('npcNameplate').length, 0);
  const named = Npc.create({ soul: 'guard', nameplate: true, position: { x: 0, y: 0, z: 0 } });
  assert.deepEqual(sent('npcNameplate').pop(), {
    op: 'npcNameplate',
    id: named.id,
    field: '11;FFFFFFFF;',
  });
  named.setNameplateText('Captain\nof the guard');
  assert.equal(named.getNameplateText(), 'Captainof the guard');
  named.setNameplateColor(0xffff0000);
  named.setNameplateHealthVisible(false);
  assert.equal(sent('npcNameplate').pop().field, '10;FFFF0000;Captainof the guard');
  const before = sent('npcNameplate').length;
  named.setNameplateHealthVisible(false);
  assert.equal(sent('npcNameplate').length, before, 'no change, no message');
  named.nameplate = false;
  assert.equal(named.isNameplateVisible(), false);
  assert.equal(sent('npcNameplate').pop().field, '');
  assert.throws(() => named.setNameplateColor(-1), RangeError);
  assert.throws(() => named.setNameplateText(5), TypeError);
});

test('locomotion and faction are checked and kept', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 }, locomotion: 'kinematic', faction: 12 });
  assert.equal(npc.locomotion, 'kinematic');
  assert.equal(npc.faction, 12);
  assert.deepEqual(sent('npcLocomotion').pop(), {
    op: 'npcLocomotion',
    id: npc.id,
    kinematic: true,
  });
  npc.locomotion = 'native';
  assert.equal(sent('npcLocomotion').pop().kinematic, false);
  assert.throws(() => {
    npc.locomotion = 'Native';
  }, TypeError);
  npc.faction = 0;
  assert.equal(npc.faction, 0);
  assert.throws(() => {
    npc.faction = 1.5;
  }, RangeError);
});

test('a hen moves kinematically unless Npc.create says otherwise', async () => {
  const { Npc } = await startedHost();
  const hen = Npc.create({ class: 'Hen', position: { x: 0, y: 0, z: 0 } });
  assert.equal(hen.locomotion, 'kinematic');
  const walking = Npc.create({
    class: 'Hen',
    locomotion: 'native',
    position: { x: 1, y: 0, z: 0 },
  });
  assert.equal(walking.locomotion, 'native');
  const ewe = Npc.create({ class: 'SheepEwe', position: { x: 2, y: 0, z: 0 } });
  assert.equal(ewe.locomotion, 'native');
});

test('attackable false takes the lock-on with it and comes back', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.attackable, true);
  assert.equal(sent('npcAttackable').length, 0, 'the default sends nothing');
  npc.attackable = false;
  assert.deepEqual(sent('npcAttackable').pop(), {
    op: 'npcAttackable',
    id: npc.id,
    attackable: false,
  });
  assert.equal(sent('npcTargetable').pop().targetable, false);
  assert.equal(npc.targetable, true, 'its own setting is kept');
  npc.attackable = true;
  assert.equal(sent('npcAttackable').pop().attackable, true);
  assert.equal(sent('npcTargetable').pop().targetable, true);
  assert.throws(() => {
    npc.attackable = 'no';
  }, TypeError);
  const shopkeeper = Npc.create({ position: { x: 1, y: 0, z: 0 }, attackable: false });
  assert.equal(shopkeeper.attackable, false);
  assert.equal(sent('npcAttackable').pop().id, shopkeeper.id);
});

test('brain and targetable switch off and on, checked and sent', async () => {
  const { Npc, sent } = await startedHost();
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(npc.brain, true);
  assert.equal(npc.targetable, true);
  assert.equal(sent('npcBrain').length, 0, 'the defaults send nothing');
  assert.equal(sent('npcTargetable').length, 0);
  npc.brain = false;
  npc.targetable = false;
  assert.deepEqual(sent('npcBrain').pop(), { op: 'npcBrain', id: npc.id, brain: false });
  assert.deepEqual(sent('npcTargetable').pop(), {
    op: 'npcTargetable',
    id: npc.id,
    targetable: false,
  });
  const before = sent('npcBrain').length;
  npc.brain = false;
  assert.equal(sent('npcBrain').length, before, 'no change, no message');
  // No brain, no revenge: fightsBack reads false whatever it was set to, and attack is refused.
  npc.fightsBack = true;
  assert.equal(npc.fightsBack, false);
  assert.equal(npc.attack(npc), false);
  npc.brain = true;
  assert.equal(npc.fightsBack, true);
  assert.throws(() => {
    npc.brain = 'no';
  }, TypeError);
  assert.throws(() => {
    npc.targetable = 1;
  }, TypeError);
  const quiet = Npc.create({ position: { x: 1, y: 0, z: 0 }, brain: false, targetable: false });
  assert.equal(quiet.brain, false);
  assert.equal(quiet.targetable, false);
  assert.deepEqual(sent('npcBrain').pop(), { op: 'npcBrain', id: quiet.id, brain: false });
  assert.deepEqual(sent('npcTargetable').pop(), {
    op: 'npcTargetable',
    id: quiet.id,
    targetable: false,
  });
  assert.throws(() => Npc.create({ position: { x: 0, y: 0, z: 0 }, brain: 'x' }), TypeError);
});

test('pin sends the player and ends when that player leaves', async () => {
  const { host, Npc, sent } = await startedHost();
  const player = host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  npc.pin(player);
  assert.deepEqual(sent('npcPin').pop(), { op: 'npcPin', id: npc.id, player: 7 });
  assert.equal(npc.pinnedTo, player);
  npc.pin(null);
  assert.equal(sent('npcPin').pop().player, 0);
  npc.pin(7);
  host.disconnect(7);
  assert.equal(npc.pinnedTo, null);
  assert.throws(() => npc.pin(99), TypeError);
  assert.throws(() => npc.pin({ id: 7 }), TypeError);
});

test('npcAnimationEnd says how each request ended, once', async () => {
  const { host, Npc } = await startedHost();
  const ends = [];
  host.on('npcAnimationEnd', (npc, fragment, reason) => ends.push([npc.id, fragment, reason]));
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  npc.playAnimation('', { clip: 'relaxed_idle_wave' });
  npc.playAnimation('', { clip: 'relaxed_idle_point' });
  assert.deepEqual(ends, [[npc.id, '', 'replaced']]);
  // A report of the replaced request is dropped; the current one ends once.
  host.npcs.animationEnded({ id: npc.id, sequence: 1, reason: 'finished' });
  host.npcs.animationEnded({ id: npc.id, sequence: 2, reason: 'finished' });
  host.npcs.animationEnded({ id: npc.id, sequence: 2, reason: 'finished' });
  assert.deepEqual(ends.at(-1), [npc.id, '', 'finished']);
  assert.equal(ends.length, 2);
  npc.playAnimation('', { clip: 'relaxed_idle_wave', loop: true });
  npc.stopAnimation();
  assert.deepEqual(ends.at(-1), [npc.id, '', 'stopped']);
  npc.stopAnimation();
  assert.equal(ends.length, 3, 'nothing playing, nothing ends');
  npc.playAnimation('', { clip: 'nothing_like_it' });
  host.npcs.animationEnded({ id: npc.id, sequence: 4, reason: 'refused' });
  assert.deepEqual(ends.at(-1), [npc.id, '', 'refused']);
});

test('a dead animal, not lootable and with nothing to harvest, closes its carcass', async (t) => {
  const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
  const f = serverFixture(t);
  await f.host.startAll();
  const { Npc } = f.host.makeAPI();
  const closed = () =>
    f.sent.filter((message) => message.op === 'npcCarcassClosed').map((message) => message.closed);
  Npc.setHarvestTable({ RoeDeerBuck: [{ item: 'apple', amount: 1 }] });
  const deer = Npc.create({
    class: 'RoeDeerBuck',
    lootable: false,
    position: { x: 0, y: 0, z: 0 },
  });
  assert.deepEqual(closed(), [], 'alive: nothing to close');
  deer.kill();
  assert.equal(deer.harvestable, true);
  assert.deepEqual(closed(), [], 'its class is in the table: Gut stays');
  Npc.setHarvestTable({});
  assert.deepEqual(closed(), [true], 'no table row: closed');
  deer.lootable = true;
  assert.deepEqual(closed(), [true, false], 'lootable: Search is back');
  deer.lootable = false;
  deer.revive(50);
  assert.deepEqual(closed(), [true, false, true, false], 'alive again: open');
});

test('useObject walks an NPC to a seat, plays the game fragments and reports start and end', async () => {
  const { host, Npc, sent } = await startedHost();
  const { SmartObject } = host.makeAPI();
  const bench = SmartObject.spawn({ blueprint: 'Bench/bench_2places_low', position: [5, 0, 0] });
  const events = [];
  host.on('npcObjectUseStarted', (npc, object, place) => events.push(['started', npc.id, place]));
  host.on('npcObjectUseEnded', (npc, object, place, reason) =>
    events.push(['ended', npc.id, place, reason]),
  );
  const sitter = Npc.create({ position: { x: 5, y: 5, z: 0 } });
  const lastAnimation = () => sent('npcAnimation').at(-1).animation.split('|');
  const finish = (npc) => {
    const [sequence] = lastAnimation();
    host.npcs.animationEnded({ id: npc.id, sequence: Number(sequence), reason: 'finished' });
  };
  const intentSequence = () => Number(sent('npcIntent').at(-1).intent.split('|')[0]);

  assert.equal(sitter.useObject(bench), true);
  assert.deepEqual(sitter.usingObject, { object: bench, place: 1 });
  // The seat is 0.35 m along the bench and turned around, its front helper 1.37 m in front of it.
  // The game's sit-down from the front moves the body 0.25 m back onto the seat, so it walks to
  // 0.25 m in front of the seat, facing away from it, and the sit-down starts there.
  const walk = sent('npcIntent').at(-1).intent.split('|');
  assert.deepEqual(walk.slice(1, 4), ['moveTo', '5.35', '0.25']);
  host.npcs.intentDone({ id: sitter.id, status: 'reached', sequence: intentSequence() });
  assert.ok(Math.abs(sitter.position.x - 5.35) < 1e-3 && Math.abs(sitter.position.y - 0.25) < 1e-3);
  assert.ok(Math.abs(sitter.rotation.z) < 1e-3 && Math.abs(Math.abs(sitter.rotation.w) - 1) < 1e-3);
  // The walk ended there, so the body does not walk on from there.
  assert.equal(sent('npcIntent').at(-1).intent, '');
  assert.equal(sitter.frozen, false);
  assert.deepEqual(lastAnimation().slice(1), [
    '0',
    '',
    'SitDown',
    'sitSideFront+sitPlaceBench+noTable',
  ]);
  finish(sitter);
  assert.deepEqual(lastAnimation().slice(1), ['1', '', 'SittingIdle', 'sittingNoTable']);
  assert.deepEqual(events, [['started', sitter.id, 1]]);

  // The second seat goes to the next NPC; then none is free.
  const second = Npc.create({ position: { x: 5, y: 5, z: 0 } });
  assert.equal(second.useObject(bench), true);
  assert.equal(second.usingObject.place, 2);
  assert.equal(Npc.create({ position: { x: 5, y: 5, z: 0 } }).useObject(bench), false);
  assert.equal(sitter.useObject(bench), false, 'it sits already');
  assert.throws(() => sitter.useObject(bench, { place: 9 }), RangeError);
  assert.throws(() => sitter.useObject({ id: 1 }), TypeError);

  // Standing up: the game's stand-up, then the end.
  assert.equal(sitter.stopUsingObject(), true);
  assert.deepEqual(lastAnimation().slice(1), ['0', '', 'StandUp', 'sitSideFront+sittingNoTable']);
  finish(sitter);
  assert.equal(sitter.usingObject, null);
  assert.equal(sitter.stopUsingObject(), false);
  // The game's stand-up carries it out of the seat: nothing moves it.
  assert.ok(Math.abs(sitter.position.y - 0.25) < 1e-3);

  // A script's own order ends the use of the one still walking there; its seat is free again.
  second.moveTo({ x: 9, y: 9, z: 0 });
  assert.equal(second.usingObject, null);
  assert.deepEqual(events.slice(1), [
    ['ended', sitter.id, 1, 'stopped'],
    ['ended', second.id, 2, 'interrupted'],
  ]);
  assert.equal(sitter.useObject(bench, { place: 2 }), true);
  // A walk that cannot get there ends as blocked.
  host.npcs.intentDone({ id: sitter.id, status: 'blocked', sequence: intentSequence() });
  assert.deepEqual(events.at(-1), ['ended', sitter.id, 2, 'blocked']);
  // A seat whose approach point is off the map: no walk, and the seat stays free.
  const edge = SmartObject.spawn({ blueprint: 'Bench/bench_2places_low', position: [0, 0, 0] });
  assert.equal(sitter.useObject(edge, { place: 2 }), false);
  assert.equal(sitter.usingObject, null);
  assert.equal(sitter.useObject(edge, { place: 1 }), true);
});
