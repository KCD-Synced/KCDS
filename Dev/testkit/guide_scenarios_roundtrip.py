"""Two native UDP clients and real WebView workers play the 0.1.1 guide walkthroughs together.

One resource combines the networking, shared state and dimension guides the way a game mode does:
clients ask the server at resourceStart, the server answers with ordered events and JSON payloads,
keeps teams in shared state with every scope, splits the players into two worlds, changes state while
they cannot see each other, and brings them back.
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

SERVER_SCRIPT = """
'use strict';
const RESOURCE = 'guide-check';
const GLOBAL_WORLD = 4294967295;
const SEQUENCE = 20;
const greeted = new Set();
const reports = new Map();
let phase = 0;
let finished = false;
let waited = 0;

function fail(text) {
  console.log('GUIDE FAIL ' + text);
}

function players() {
  return kcds.players.Player.all().sort((left, right) => left.id - right.id);
}

// Networking guide: a client asks once its scripts run; the sender is the connection.
kcds.core.Events.fromClient(RESOURCE + ':hello', (sender, payload) => {
  // A plain string that is not JSON text arrives as that string.
  if (sender instanceof kcds.players.Player && payload === 'hello') {
    console.log('GUIDE HELLO TEXT');
    return;
  }
  if (!(sender instanceof kcds.players.Player) || payload !== null) {
    fail('hello sender=' + String(sender) + ' payload=' + JSON.stringify(payload));
    return;
  }
  greeted.add(sender.id);
});

kcds.core.Events.fromClient(RESOURCE + ':report', (sender, data) => {
  if (data.secretLeak || data.ownerLeak) {
    fail('private state reached player=' + sender.id + ' phase=' + data.phase);
  }
  reports.set(sender.id, data);
});

const phases = [
  {
    // Networking and state: answer both players, keep teams in shared state with every scope.
    enter(a, b) {
      a.state.set('team', 'ravens');
      b.state.set('team', 'wolves');
      for (const player of [a, b]) {
        player.state.set('score', 0);
        player.state.set('wanted', player.id, { scope: 'owner' });
        player.state.set('joinedAt', Date.now(), { scope: 'server' });
      }
      const rows = players().map((p) => ({ id: p.id, team: p.state.get('team') }));
      kcds.core.Events.broadcast(RESOURCE + ':scores', JSON.stringify(rows));
      for (let n = 1; n <= SEQUENCE; n += 1) {
        kcds.core.Events.broadcast(RESOURCE + ':seq', n);
      }
      kcds.core.Events.broadcast(RESOURCE + ':payload', {
        spot: new kcds.math.Vector3(1, 2, 3),
        facing: kcds.math.Quaternion.identity(),
        skip: undefined,
        run() {},
      });
    },
    expect(a, b, reportA, reportB) {
      return [reportA, reportB].every((report) => {
        return report.ordered && report.plainPayload && report.rows === 2 &&
          report.ownTeam && report.ownWanted && report.otherTeam;
      });
    },
  },
  {
    // Dimensions: split the players, then change state while they cannot see each other.
    enter(a, b) {
      a.setDimension(1);
      b.setDimension(2);
      a.state.set('badge', 'champion');
      b.state.set('score', 5);
    },
    expect(a, b, reportA, reportB) {
      return !reportA.sees && !reportB.sees && reportA.localWorld === 1 && reportB.localWorld === 2;
    },
  },
  {
    // Back to the global world, with A's team narrowed to its owner.
    enter(a, b) {
      a.state.set('team', 'ravens', { scope: 'owner' });
      a.setDimension(GLOBAL_WORLD);
      b.setDimension(GLOBAL_WORLD);
    },
    expect(a, b, reportA, reportB) {
      return (
        reportA.sees && reportB.sees &&
        reportA.localWorld === GLOBAL_WORLD && reportB.localWorld === GLOBAL_WORLD &&
        reportB.otherBadge === 'champion' && reportB.otherHasTeam === false &&
        reportA.ownTeam && reportA.otherScore === 5
      );
    },
  },
];

setInterval(() => {
  const connected = players();
  kcds.core.Events.broadcast(RESOURCE + ':phase', {
    phase,
    ids: connected.map((player) => player.id),
  });
  if (finished || connected.length !== 2) {
    return;
  }
  const [a, b] = connected;
  if (phase === 0) {
    if (!greeted.has(a.id) || !greeted.has(b.id)) {
      return;
    }
    phase = 1;
    phases[0].enter(a, b);
    return;
  }
  const reportA = reports.get(a.id);
  const reportB = reports.get(b.id);
  if (!reportA || !reportB || reportA.phase !== phase || reportB.phase !== phase) {
    return;
  }
  if (!phases[phase - 1].expect(a, b, reportA, reportB)) {
    waited += 1;
    if (waited % 10 === 0) {
      console.log('GUIDE WAIT phase=' + phase + ' a=' + JSON.stringify(reportA) +
        ' b=' + JSON.stringify(reportB));
    }
    return;
  }
  waited = 0;
  console.log('GUIDE PHASE ' + phase + ' passed');
  if (phase === phases.length) {
    finished = true;
    console.log('GUIDE COMPLETE');
    for (const player of connected) {
      kcds.core.Chat.sendToPlayer(player, 'resource-roundtrip-ok');
    }
    return;
  }
  phase += 1;
  phases[phase - 1].enter(a, b);
}, 200);
"""

CLIENT_SCRIPT = """
'use strict';
const RESOURCE = 'guide-check';
let phase = 0;
let ids = [];
let rows = 0;
const sequence = [];
let plainPayload = false;

kcds.core.Events.on(RESOURCE + ':phase', (payload) => {
  phase = payload.phase;
  ids = payload.ids;
});
kcds.core.Events.on(RESOURCE + ':scores', (payload) => {
  if (Array.isArray(payload)) {
    rows = payload.length;
  }
});
kcds.core.Events.on(RESOURCE + ':seq', (n) => {
  sequence.push(n);
});
kcds.core.Events.on(RESOURCE + ':payload', (payload) => {
  const text = JSON.stringify(payload);
  plainPayload =
    text === '{"spot":{"x":1,"y":2,"z":3},"facing":{"w":1,"x":0,"y":0,"z":0}}' &&
    typeof payload.spot.clone === 'undefined';
});
kcds.core.Events.on('resourceStart', (name) => {
  if (name === RESOURCE) {
    // A bare string is not JSON text: it arrives as that string.
    kcds.core.Events.toServer(RESOURCE + ':hello', 'hello');
    kcds.core.Events.toServer(RESOURCE + ':hello');
  }
});

function visible(id) {
  try {
    return new kcds.players.Entity(id);
  } catch {
    return null;
  }
}

setInterval(() => {
  const self = kcds.players.LocalPlayer;
  if (!phase || !self || ids.length !== 2) {
    return;
  }
  const otherId = ids.find((id) => id !== self.id);
  const other = visible(otherId);
  const ordered =
    sequence.length === 20 && sequence.every((value, index) => value === index + 1);
  kcds.core.Events.toServer(RESOURCE + ':report', {
    phase,
    ordered,
    plainPayload,
    rows,
    localWorld: self.dimension,
    sees: other !== null,
    ownTeam: typeof self.state.get('team') === 'string',
    ownWanted: self.state.get('wanted') === self.id,
    otherTeam: other !== null && typeof other.state.get('team') === 'string',
    otherHasTeam: other !== null && other.state.has('team'),
    otherBadge: other === null ? null : other.state.get('badge') ?? null,
    otherScore: other === null ? null : other.state.get('score') ?? null,
    secretLeak: self.state.has('joinedAt') || (other !== null && other.state.has('joinedAt')),
    ownerLeak: other !== null && other.state.has('wanted'),
  });
}, 100);
"""


def run(server_exe, probe_exe, runtime_exe, node_exe, work):
    resource = work / "resources" / "guide-check"
    resource.mkdir(parents=True)
    (resource / "package.json").write_text(
        json.dumps(
            {
                "name": "guide-check",
                "kcds": {
                    "serverScripts": ["server.cjs"],
                    "clientScripts": ["client.cjs"],
                },
            }
        ),
        encoding="utf8",
    )
    (resource / "server.cjs").write_text(SERVER_SCRIPT, encoding="utf8")
    (resource / "client.cjs").write_text(CLIENT_SCRIPT, encoding="utf8")
    # Reserve a free loopback UDP port. A competing bind fails this test safely.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = str(reservation.getsockname()[1])
    flags = subprocess.CREATE_NO_WINDOW
    server_log_path = work / "server.log"
    # Every server requires a legal game: a test Master and a join pass for each
    # client (licence_fixture.py).
    with LicenceFixture(work, resource.parent) as licence, server_log_path.open(
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
            while "RESOURCE HOST ready" not in server_log_path.read_text(encoding="utf8"):
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
                    probe.wait(timeout=40)
                finally:
                    if probe.poll() is None:
                        probe.terminate()
                        probe.wait(timeout=5)
            print((work / "probe.log").read_text(encoding="utf8"))
            log = server_log_path.read_text(encoding="utf8")
            if "GUIDE FAIL" in log:
                raise RuntimeError("Guide scenario violation reported by the resource")
            if probe.returncode or "GUIDE COMPLETE" not in log:
                raise RuntimeError("Guide scenario roundtrip did not complete")
            if log.count("GUIDE HELLO TEXT") < 2:
                raise RuntimeError("A plain-text client payload did not arrive as text")
            print(log)
        except Exception:
            print(server_log_path.read_text(encoding="utf8"))
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
        raise SystemExit("Usage: guide_scenarios_roundtrip.py <server> <probe> <runtime> <node>")
    with tempfile.TemporaryDirectory(prefix="kcds-guide-roundtrip-") as folder:
        run(*sys.argv[1:], Path(folder))
