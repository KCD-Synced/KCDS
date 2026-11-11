'use strict';
// Workload resource of the KCDS capacity benchmark (Dev/testkit/capacity_benchmark.py).
// The benchmark drives it through server console commands:
//
//   capacity shared_state <hz>  sets a broadcast and an owner-scoped SharedState key on every player
//                           <hz> times per second (0 stops)
//   capacity events <hz>    broadcasts the resource event kcds-capacity-ping with the sender's
//                           unix time in milliseconds <hz> times per second (0 stops)
//   capacity idle           stops both
//
// Every change is logged once so the benchmark can confirm the workload actually ran.

const { Events } = kcds.core;
const { Player } = kcds.players;

const timers = { shared_state: null, events: null };
let generation = 0;

function stop(kind) {
  if (timers[kind] !== null) {
    clearInterval(timers[kind]);
    timers[kind] = null;
  }
}

function start(kind, hz, work) {
  stop(kind);
  if (!(hz > 0)) {
    console.log(`capacity ${kind} stopped`);
    return;
  }
  const period = Math.max(1, Math.round(1000 / hz));
  timers[kind] = setInterval(work, period);
  console.log(`capacity ${kind} every ${period} ms`);
}

function mutateSharedStates() {
  generation += 1;
  for (const player of Player.all()) {
    player.state.set('capacityTick', generation);
    player.state.set('capacityOwner', generation, { scope: 'owner' });
  }
}

function broadcastPing() {
  generation += 1;
  Events.broadcast('kcds-capacity-ping', { t: Date.now(), n: generation });
}

Events.on('consoleCommand', (command, args) => {
  if (command !== 'capacity') {
    return;
  }
  const mode = args[0];
  const hz = Number(args[1] ?? 0);
  if (mode === 'shared_state') {
    start('shared_state', hz, mutateSharedStates);
  } else if (mode === 'events') {
    start('events', hz, broadcastPing);
  } else if (mode === 'idle') {
    stop('shared_state');
    stop('events');
    console.log('capacity idle');
  } else {
    console.log(`capacity: unknown mode ${mode}`);
  }
});
