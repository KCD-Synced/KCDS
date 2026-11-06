'use strict';
// The client half of the example gamemode: the character creator /creator opens (server.js) and
// NoClip on F3.

// F3 flies (NoClip, the body with the camera); F3 again lands where the flight ends. A player
// the server does not let fly (kcds.players.Player.noclipAllowed) is told so.
kcds.ui.Key.bind('f3', () => {
  if (kcds.ui.NoClip.isActive()) {
    kcds.ui.NoClip.disable();
  } else if (kcds.ui.NoClip.enable() === 'notAllowed') {
    kcds.ui.Hud.showNotification('Flying is not allowed on this server.');
  }
});

kcds.core.Events.on('freeroam:creator', (request) => {
  if (!kcds.players.LocalPlayer || request?.player !== kcds.players.LocalPlayer.id) {
    return;
  }
  kcds.ui.CharacterCreator.open({ genders: request.genders, title: 'Your Character' });
});

kcds.core.Events.on('freeroam:creatorStop', (request) => {
  if (kcds.players.LocalPlayer && request?.player === kcds.players.LocalPlayer.id) {
    kcds.ui.CharacterCreator.close();
  }
});

// Only an accepted look goes to the server; the other endings are said in the chat.
kcds.core.Events.on('characterCreatorClosed', (appearance, reason) => {
  if (appearance !== null) {
    kcds.core.Events.toServer('freeroam:look', appearance);
  } else if (reason !== 'replaced') {
    kcds.ui.Hud.showNotification(`Character creator closed (${reason}).`);
  }
});
