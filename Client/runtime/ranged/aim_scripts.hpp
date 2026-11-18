// Whether the local player aims a bow, a crossbow or a gun (the game's aim pose is on) or works a
// siege engine, for the client scripts: { op: 'localAim', aiming } to the resource runtime on each
// change, again when the runtime starts anew. The scripts raise localPlayerAimStart and
// localPlayerAimEnd and read LocalPlayer.aiming; Hud's "rangedOnly" cursor follows it
// (Client/resources/hud.cjs).
//
// Engine thread, without the network lock.
#pragma once

namespace kcds::game_client {

void tell_scripts_aiming(bool aiming);

}  // namespace kcds::game_client
