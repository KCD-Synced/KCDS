"""Two native UDP clients and real WebView workers validate Player dimensions
and private visibility through the production resource host."""

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
const GLOBAL_WORLD = 4294967295;
let phase = 0;
let finished = false;
let waited = 0;
const reports = new Map();

function fail(text) {
  console.log('VISIBILITY FAIL ' + text);
}

kcds.core.Events.on('playerConnect', (player) => {
  player.state.set('public', 'visible-' + player.id);
  player.state.set('owner', player.id, { scope: 'owner' });
  player.state.set('secret', 'server-only', { scope: 'server' });
});

kcds.core.Events.fromClient('visibility-report', (player, data) => {
  if (data.secretLeak || data.ownerLeak) {
    fail('private state reached player=' + player.id + ' phase=' + data.phase);
  }
  reports.set(player.id, data);
});

function players() {
  return kcds.players.Player.all().sort((left, right) => left.id - right.id);
}

// Each phase applies its server mutation once, then waits until both clients report
// the expected view for that phase.
const phases = [
  {
    enter() {},
    expect(a, b, reportA, reportB) {
      return reportA.sees && reportB.sees && reportA.otherPublic && reportB.otherPublic;
    },
  },
  {
    enter(a, b) {
      a.setDimension(1);
      b.setDimension(2);
      if (a.dimension !== 1 || b.dimension !== 2) {
        fail('dimension read did not follow setDimension');
      }
    },
    expect(a, b, reportA, reportB) {
      return (
        !reportA.sees &&
        !reportB.sees &&
        reportA.localWorld === 1 &&
        reportB.localWorld === 2 &&
        a.dimension === 1 &&
        b.dimension === 2
      );
    },
  },
  {
    enter(a, b) {
      b.setDimension(1);
    },
    expect(a, b, reportA, reportB) {
      return reportA.sees && reportB.sees && reportA.otherPublic && reportB.otherPublic;
    },
  },
  {
    enter(a, b) {
      a.setDimension(GLOBAL_WORLD);
      b.setDimension(GLOBAL_WORLD);
      a.showOnlyTo(a);
    },
    expect(a, b, reportA, reportB) {
      return (
        reportA.sees &&
        !reportB.sees &&
        reportA.localWorld === GLOBAL_WORLD &&
        a.dimension === GLOBAL_WORLD &&
        b.dimension === GLOBAL_WORLD
      );
    },
  },
  {
    enter(a, b) {
      a.showOnlyTo(b);
    },
    expect(a, b, reportA, reportB) {
      return reportA.sees && reportB.sees && reportB.otherPublic;
    },
  },
  {
    enter(a, b) {
      a.showOnlyTo(null);
      b.showOnlyTo(b);
    },
    expect(a, b, reportA, reportB) {
      return !reportA.sees && reportB.sees;
    },
  },
  {
    enter(a, b) {
      b.showOnlyTo(null);
    },
    expect(a, b, reportA, reportB) {
      return reportA.sees && reportB.sees && reportA.otherPublic && reportB.otherPublic;
    },
  },
];

setInterval(() => {
  const connected = players();
  kcds.core.Events.broadcast('visibility-phase', {
    phase,
    ids: connected.map((player) => player.id),
  });
  if (finished || connected.length !== 2) {
    return;
  }
  const [a, b] = connected;
  if (phase === 0) {
    phase = 1;
    phases[0].enter(a, b);
    return;
  }
  const reportA = reports.get(a.id);
  const reportB = reports.get(b.id);
  if (!reportA || !reportB || reportA.phase !== phase || reportB.phase !== phase) {
    return;
  }
  if (!reportA.self || !reportB.self || !reportA.ownerState || !reportB.ownerState) {
    return;
  }
  if (!phases[phase - 1].expect(a, b, reportA, reportB)) {
    waited += 1;
    if (waited % 10 === 0) {
      console.log('VISIBILITY WAIT phase=' + phase + ' a=' + JSON.stringify(reportA) +
        ' b=' + JSON.stringify(reportB));
    }
    return;
  }
  waited = 0;
  console.log('VISIBILITY PHASE ' + phase + ' passed');
  if (phase === phases.length) {
    finished = true;
    console.log('VISIBILITY COMPLETE');
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
let phase = 0;
let ids = [];
kcds.core.Events.on('visibility-phase', (payload) => {
  phase = payload.phase;
  ids = payload.ids;
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
  kcds.core.Events.toServer('visibility-report', {
    phase,
    self: self.id,
    localWorld: self.dimension,
    ownerState: self.state.get('owner') === self.id,
    sees: other !== null,
    otherPublic: other !== null && other.state.get('public') === 'visible-' + otherId,
    secretLeak: self.state.has('secret') || (other !== null && other.state.has('secret')),
    ownerLeak: other !== null && other.state.has('owner'),
  });
}, 100);
"""


def run(server_exe, probe_exe, runtime_exe, node_exe, work):
    resource = work / "resources" / "visibility-check"
    resource.mkdir(parents=True)
    (resource / "package.json").write_text(
        json.dumps(
            {
                "name": "visibility-check",
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
            if "VISIBILITY FAIL" in log:
                raise RuntimeError("Visibility contract violation reported by the resource")
            if probe.returncode or "VISIBILITY COMPLETE" not in log:
                raise RuntimeError("Player visibility roundtrip did not complete")
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
        raise SystemExit("Usage: player_visibility_roundtrip.py <server> <probe> <runtime> <node>")
    with tempfile.TemporaryDirectory(prefix="kcds-visibility-roundtrip-") as folder:
        run(*sys.argv[1:], Path(folder))
