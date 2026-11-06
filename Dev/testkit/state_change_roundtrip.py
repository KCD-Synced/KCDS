"""Two native UDP clients and real WebView workers validate Entity handles and SharedState changes.

Covers server Entity/Player identity, client keys/toObject/scopes, server writes and removals, and
client onChange delivery of a removal and a new key.
"""

import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from licence_fixture import LicenceFixture  # noqa: E402  (a join pass for each client)


def run(server_exe, probe_exe, runtime_exe, node_exe, work):
    resource = work / "resources" / "state-check"
    resource.mkdir(parents=True)
    (resource / "package.json").write_text(
        json.dumps(
            {
                "name": "state-check",
                "kcds": {
                    "serverScripts": ["server.cjs"],
                    "clientScripts": ["client.cjs"],
                },
            }
        ),
        encoding="utf8",
    )
    (resource / "server.cjs").write_text(
        """
    kcds.core.Events.on('playerConnect', player => {
      player.state.set('public', 'čau');
      player.state.set('owner', player.id, { scope: 'owner' });
      player.state.set('secret', 'private-server', { scope: 'server' });
    });
    setInterval(() => {
      kcds.core.Events.broadcast('test-ids', kcds.players.Player.all().map(player => player.id));
    }, 200);
    const fail = (player, text) => {
      console.log('STATE FAIL player=' + player.id + ' ' + text);
      throw Error(text);
    };
    const changed = new Set();
    // EventMap.entityStateChange(entity, key, value, previous) on the server, for script writes.
    const serverEvents = new Map();
    kcds.core.Events.on('entityStateChange', (entity, key, value, previous) => {
      if (!(entity instanceof kcds.players.Entity)) {
        throw Error('entityStateChange entity is not an Entity');
      }
      const list = serverEvents.get(entity.id) ?? [];
      list.push([key, value === undefined ? 'undefined' : value, previous === undefined ? 'undefined' : previous]);
      serverEvents.set(entity.id, list);
    });
    kcds.core.Events.fromClient('state-ready', (player, data) => {
      // Server Entity handle: wraps the existing player and is the base of Player.
      if (!(player instanceof kcds.players.Entity) || new kcds.players.Entity(player.id) !== player ||
          String(player) !== 'Player(' + player.id + ', ' + JSON.stringify(player.nickname) + ')' ||
          !(player.position instanceof kcds.math.Vector3) || !(player.rotation instanceof kcds.math.Quaternion) ||
          player.dimension !== 4294967295 || !(player.state instanceof kcds.core.SharedState)) {
        fail(player, 'server Entity');
      }
      // Client view: owner keys reach their owner only, server keys never travel.
      if (data.id !== player.id || data.keys.join() !== 'owner,public' ||
          JSON.stringify(data.object) !== JSON.stringify({ owner: player.id, public: 'čau' }) ||
          data.otherKeys.some(key => key === 'owner' || key === 'secret') ||
          data.text !== 'Player(' + player.id + ', ' + JSON.stringify(player.nickname) + ', local)' || !data.vector || !data.quaternion ||
          data.world !== 4294967295 || !data.sameId) {
        fail(player, 'client view ' + JSON.stringify(data));
      }
      if (changed.has(player.id)) {
        return;
      }
      changed.add(player.id);
      // Server SharedState: all keys are visible to the server, writes report whether they changed.
      const state = player.state;
      if (state.keys().join() !== 'owner,public,secret' || !state.has('secret') ||
          state.get('secret') !== 'private-server' ||
          JSON.stringify(state.toObject()) !==
            JSON.stringify({ owner: player.id, public: 'čau', secret: 'private-server' }) ||
          state.set('public', 'čau') !== false || state.remove('missing') !== false) {
        fail(player, 'server SharedState');
      }
      if (state.remove('public') !== true || state.set('counter', 2) !== true) {
        fail(player, 'server SharedState write');
      }
    });
    kcds.core.Events.fromClient('state-changed', (player, data) => {
      const expected = [['public', null, 'čau'], ['counter', 2, null]];
      if (JSON.stringify(data.events) !== JSON.stringify(expected) ||
          data.hasPublic !== false || data.counter !== 2) {
        fail(player, 'client onChange ' + JSON.stringify(data));
      }
      const own = JSON.stringify(serverEvents.get(player.id) ?? []);
      if (!own.includes(JSON.stringify(['public', 'undefined', 'čau'])) ||
          !own.includes(JSON.stringify(['counter', 2, 'undefined']))) {
        fail(player, 'server entityStateChange ' + own);
      }
      const clientEvents = JSON.stringify(data.events2);
      if (!data.sharedStateClass || !clientEvents.includes(JSON.stringify(['public', 'undefined', 'čau'])) ||
          !clientEvents.includes(JSON.stringify(['counter', 2, 'undefined']))) {
        fail(player, 'client entityStateChange ' + clientEvents);
      }
      console.log('STATE CHANGE OK player=' + player.id);
      kcds.core.Chat.sendToPlayer(player, 'resource-roundtrip-ok');
    });
""",
        encoding="utf8",
    )
    (resource / "client.cjs").write_text(
        """
    let ids = [];
    let readySent = false;
    let changedSent = false;
    const events = [];
    // EventMap.entityStateChange(entity, key, value, previous) on the client, when a write arrives.
    const events2 = [];
    kcds.core.Events.on('entityStateChange', (entity, key, value, previous) => {
      const player = kcds.players.LocalPlayer;
      if (readySent && player && entity instanceof kcds.players.Entity && entity.id === player.id) {
        events2.push([key, value === undefined ? 'undefined' : value, previous === undefined ? 'undefined' : previous]);
      }
    });
    kcds.core.Events.on('test-ids', payload => {
      ids = payload;
    });
    setInterval(() => {
      const player = kcds.players.LocalPlayer;
      if (!player || ids.length !== 2) {
        return;
      }
      if (!readySent) {
        if (player.state.get('public') !== 'čau' || player.state.get('owner') !== player.id) {
          return;
        }
        let other;
        try {
          other = new kcds.players.Entity(ids.find(id => id !== player.id));
        } catch {
          return;
        }
        player.state.onChange(null, (key, value, previous) => {
          events.push([key, value, previous]);
        });
        readySent = true;
        kcds.core.Events.toServer('state-ready', {
          id: player.id,
          keys: player.state.keys(),
          object: player.state.toObject(),
          otherKeys: other.state.keys(),
          text: String(player),
          vector: player.position instanceof kcds.math.Vector3,
          quaternion: player.rotation instanceof kcds.math.Quaternion,
          world: player.dimension,
          sameId: new kcds.players.Entity(player.id).id === player.id,
        });
        return;
      }
      if (!changedSent && events.length >= 2) {
        changedSent = true;
        kcds.core.Events.toServer('state-changed', {
          events,
          hasPublic: player.state.has('public'),
          sharedStateClass: player.state instanceof kcds.core.SharedState,
          events2,
          counter: player.state.get('counter'),
        });
      }
    }, 100);
""",
        encoding="utf8",
    )
    # Reserve a free loopback UDP port. A competing bind fails this test safely.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = str(reservation.getsockname()[1])
    flags = subprocess.CREATE_NO_WINDOW
    # Every server requires a legal game: a test Master and a join pass for each
    # client (licence_fixture.py).
    with LicenceFixture(work, resource.parent) as licence, (work / "server.log").open(
        "w", encoding="utf8"
    ) as server_log:
        server = subprocess.Popen(
            [
                server_exe,
                port,
                "127.0.0.1",
                "trosecko",
                node_exe,
                str(ROOT / "Backend/scripting/runtimes/js/main.cjs"),
                str(resource.parent),
            ],
            stdin=subprocess.PIPE,
            stdout=server_log,
            stderr=server_log,
            text=True,
            creationflags=flags,
            env=licence.server_env(),
        )
        try:
            deadline = time.monotonic() + 10
            while "RESOURCE HOST ready" not in (work / "server.log").read_text(encoding="utf8"):
                if server.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Server did not start")
                time.sleep(0.05)
            with (work / "probe.log").open("w", encoding="utf8") as probe_log:
                probe = subprocess.Popen(
                    [probe_exe, port, runtime_exe],
                    stdout=probe_log,
                    stderr=probe_log,
                    creationflags=flags,
                    env=licence.probe_env(),
                )
                try:
                    deadline = time.monotonic() + 35
                    while probe.poll() is None:
                        if time.monotonic() > deadline:
                            raise RuntimeError("Entity/SharedState change roundtrip timed out")
                        time.sleep(0.05)
                    print((work / "probe.log").read_text(encoding="utf8"))
                    log = (work / "server.log").read_text(encoding="utf8")
                    if probe.returncode or log.count("STATE CHANGE OK player=") != 2:
                        raise RuntimeError("Entity/SharedState change roundtrip failed")
                    if "STATE FAIL" in log:
                        raise RuntimeError("Entity/SharedState change check failed")
                finally:
                    if probe.poll() is None:
                        probe.terminate()
                        probe.wait(timeout=5)
        except Exception:
            print((work / "server.log").read_text(encoding="utf8"))
            raise
        finally:
            if server.poll() is None:
                server.stdin.write("stop\n")
                server.stdin.flush()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.terminate()
                    server.wait(timeout=5)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("Usage: state_change_roundtrip.py <server> <probe> <runtime> <node>")
    with tempfile.TemporaryDirectory(prefix="kcds-state-roundtrip-") as folder:
        run(*sys.argv[1:], Path(folder))
