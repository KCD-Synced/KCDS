const RESOURCE = 'horse';

kcds.core.Events.on('playerSpawned', (player) => {
  kcds.core.Chat.sendToPlayer(player, 'T opens chat. Use /horse to spawn a horse.', {
    author: 'Server',
  });
});

kcds.core.Events.on('playerCommand', (player, command, args) => {
  if (command.toLowerCase() !== 'horse') return;
  if (!player.ready) {
    kcds.core.Chat.sendToPlayer(player, 'Wait until you are in the world.');
    return;
  }
  if (args.length > 0) {
    kcds.core.Chat.sendToPlayer(player, 'Usage: /horse');
    return;
  }
  for (const old of kcds.characters.Horse.all()) {
    if (old.ownerId === player.id) {
      if (old.mounted) {
        kcds.core.Chat.sendToPlayer(player, 'Dismount before replacing your horse.');
        return;
      }
      old.destroy();
    }
  }
  const rotation = player.rotation as kcds.math.Quaternion;
  const facing = rotation.rotateVector(new kcds.math.Vector3(0, 1, 0));
  const length = Math.sqrt(facing.x * facing.x + facing.y * facing.y);
  const direction =
    length < 1e-4
      ? new kcds.math.Vector3(0, 1, 0)
      : new kcds.math.Vector3(facing.x / length, facing.y / length, 0);
  const position = player.position.clone().add(direction.mul(3));
  const horse = kcds.characters.Horse.spawn(
    position,
    rotation,
    undefined,
    `${player.nickname}'s horse`,
  );
  horse.setDimension(player.dimension);
  horse.giveTo(player);
});

kcds.core.Events.on('resourceStart', (name) => {
  if (name === RESOURCE) console.log('/horse registered through playerCommand');
});
