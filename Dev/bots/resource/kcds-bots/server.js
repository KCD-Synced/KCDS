// kcds-bots support: a riding bot (behavior: rider) asks for a horse with the client event
// kcds-bots:horse; it gets one beside it, its own, as a player who bought one. Bots are named
// bot-<group>-<n>.<run>; anyone else asking gets nothing. Put this resource only on a test server
// that runs with KCDS_ALLOW_BOTS=1.
const { Events } = kcds.core;
const { Horse } = kcds.characters;
const { Vector3 } = kcds.math;

// One horse a bot; it goes when its bot leaves (Horse.removedWithOwner, true by default).
const given = new Map();

Events.fromClient('kcds-bots:horse', (player) => {
  if (!player.nickname.startsWith('bot-')) {
    return;
  }
  const own = given.get(player.id);
  if (own && own.alive) {
    return;
  }
  const spot = player.position.clone().add(player.rotation.rotateVector(new Vector3(0, 2, 0)));
  const horse = Horse.spawn(spot, player.rotation, '', `Horse of ${player.nickname}`);
  horse.setDimension(player.dimension);
  horse.giveTo(player);
  given.set(player.id, horse);
});

Events.on('playerDisconnect', (player) => {
  given.delete(player.id);
});
