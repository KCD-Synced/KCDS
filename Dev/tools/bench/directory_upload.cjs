'use strict';

// the bytes the resource host hands the native server for clients' entity
// directories (codec-encoded resourceReply messages, what the server relays to each client)
// while every player moves every 100 ms, how many directory updates each client gets, and the
// CPU the resource host spends on it (the sender's work: records, pumping and encoding the
// frames, acknowledgements; the simulated clients' decoding is not counted), as a percentage of
// one core.
//
//   node Dev/tools/bench/directory_upload.cjs <tree root> <players> <peers> <seconds>
//
// <tree root> is a checkout (this one, or an older one exported with git archive) so two
// versions can be compared on the same load. The first two seconds are not counted.
const path = require('node:path');
const [root, playerCount, peerCount, seconds] = [
  path.resolve(process.argv[2]),
  +process.argv[3],
  +process.argv[4],
  +process.argv[5],
];
const js = (file) => require(path.join(root, 'Backend/scripting/runtimes/js', file));
const { EntitySender } = js('entity_sender.cjs');
const { EntityStateStore } = js('shared_state.cjs');
const { encode } = js('codec.cjs');
const { progressionFromReport } = js('progression/player_progression.cjs');
const { progressionCatalog } = js('progression/catalog.cjs');
const { EntityTransfer } = require(path.join(root, 'Client/resources/entity_transfer.cjs'));
const { ClientEntities } = require(path.join(root, 'Client/resources/entities.cjs'));
const vitals = {
  health: 100,
  stamina: 126.67,
  exhaust: 99.9,
  hunger: 99.9,
  bleeding: 0,
  sleeping: 0,
  consciousness: 1,
  drunkenness: 0,
  poisoning: 0,
};
const stats = Object.fromEntries(
  [
    'charisma',
    'visibility',
    'conspicuousness',
    'noise',
    'dirtiness',
    'bloodiness',
    'smell',
    'smellIntensity',
    'fragrance',
    'carriedWeight',
    'inventoryCapacity',
    'encumbrance',
    'hangover',
    'alcoholism',
    'armorRating',
    'overallArmorDefense',
    'overallWeaponAttack',
    'normalizedRunSpeed',
    'runSpeedBase',
    'morale',
  ].map((n, i) => [n, i * 1.25]),
);
const skills = {
  fencing: 5,
  survival: 19,
  defense: 5,
  sword: 5,
  heavyWeapons: 5,
  marksmanship: 5,
  dagger: 0,
  largeWeapons: 5,
  unarmed: 5,
};
const abilities = {
  maxHealth: 100,
  maxStamina: 126.67,
  staminaCap: 126.67,
  maxExhaust: 100,
  maxHunger: 150,
  level: 5,
  strength: 5,
  agility: 5,
  vitality: 5,
  skills,
  scaledSkills: Object.fromEntries(Object.keys(skills).map((k) => [k, 0.1667])),
};
const record = (id, t) => ({
  id,
  nativeId: id,
  kind: 'player',
  dimension: 0,
  position: { x: 1600 + id + Math.sin(t + id), y: 2100 + Math.cos(t + id), z: 35 },
  rotation: { w: Math.cos(t), x: 0, y: 0, z: Math.sin(t) },
  appearance: {
    gender: 'male',
    body: 'henry_body',
    head: 'henry_head',
    hair: 'henry_hair',
    beard: 'henry_beard',
  },
  disguise: null,
  nickname: 'Player' + id,
  playerIndex: id,
  ready: true,
  vitals,
  stats,
  scaledStats: { strength: 0.1667, agility: 0.1667, vitality: 0.1667 },
  abilities,
});
const store = new EntityStateStore();
const clients = new Map();
let bytes = 0,
  messages = 0,
  updates = 0,
  warmUpdates = 0;
const sender = new EntitySender({
  store,
  maxPeers: 1024,
  session: () => '0123456789abcdef0123456789abcdef',
  send: (peer, message) => {
    bytes += encode({ op: 'resourceReply', peer, message }).length;
    messages += 1;
    if (
      message.kind === 'directory' &&
      (message.op === 'entityCommit' || message.op === 'entityFrame')
    )
      updates += 1;
    clients.get(peer).inbox.push(message);
  },
});
const ids = Array.from({ length: playerCount }, (_, i) => i + 1);
sender.applyRecords({ entities: ids.map((id) => record(id, 0)) });
const tracks = progressionCatalog.tracks().map(() => [5, 0, 0]);
for (const id of ids)
  sender.setProgression(id, progressionFromReport({ tracks, mainPoints: 1, perks: [] }));
for (let peer = 1; peer <= peerCount; ++peer) {
  const entities = new ClientEntities();
  const receiver = new EntityTransfer((kind, value) =>
    kind === 'directory' ? entities.updateDirectory(value) : entities.applyState(value),
  );
  clients.set(peer, { receiver, inbox: [] });
  for (let start = 0; start < ids.length; start += 1000)
    sender.applyViewDelta({
      peer,
      localPlayerId: peer,
      added: ids.slice(start, start + 1000),
      removed: [],
      owned: [peer],
      final: true,
    });
}
let t = 0,
  warm = 0,
  warmBytes = 0,
  senderNs = 0n,
  warmSenderNs = 0n;
// The sender's own work, timed (one thread: its wall time is its CPU time).
const timed = (work) => {
  const start = process.hrtime.bigint();
  work();
  senderNs += process.hrtime.bigint() - start;
};
const step = 0.05;
for (let tick = 0; t < seconds; ++tick, t += step) {
  if (tick % 2 === 0)
    timed(() => sender.applyRecords({ entities: ids.map((id) => record(id, t)) }));
  timed(() => sender.pump(t));
  for (const [peer, client] of clients) {
    while (client.inbox.length) {
      const message = client.inbox.shift();
      if (message.op === 'entitySession') client.receiver.reset(message.session);
      else client.receiver.receive(message);
      timed(() =>
        sender.acknowledge(peer, { session: message.session, delivery: message.delivery }),
      );
    }
  }
  if (t < 2) {
    warmBytes = bytes;
    warmUpdates = updates;
    warmSenderNs = senderNs;
    warm = t;
  }
}
const measured = seconds - 2;
const per = (bytes - warmBytes) / measured;
const ups = (updates - warmUpdates) / measured / peerCount;
console.log(
  JSON.stringify({
    players: playerCount,
    peers: peerCount,
    totalBytesPerSecond: Math.round(per),
    perPeerBytesPerSecond: Math.round(per / peerCount),
    directoryUpdatesPerPeerPerSecond: +ups.toFixed(2),
    bytesPerUpdate: Math.round(per / peerCount / ups),
    // Simulated seconds run as fast as the machine allows: the sender's milliseconds per
    // simulated second, divided by 10, is its share of one core in real time.
    senderCorePercent: +(Number(senderNs - warmSenderNs) / 1e6 / measured / 10).toFixed(2),
  }),
);
