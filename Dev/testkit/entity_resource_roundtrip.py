"""Two native UDP clients and real WebView workers validate Entity/SharedState delivery."""

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
    resource = work / "resources" / "entity-check"
    resource.mkdir(parents=True)
    (resource / "package.json").write_text(
        json.dumps(
            {
                "name": "entity-check",
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
    const generation = 1;
    kcds.core.Events.on('playerConnect', player => {
      player.state.set('public', 'čau');
      player.state.set('owner', player.id, { scope: 'owner' });
      player.state.set('secret', 'private-server', { scope: 'server' });
    });
    setInterval(() => {
      kcds.core.Events.broadcast('test-ids', kcds.players.Player.all().map(player => player.id));
    }, 200);
    kcds.core.Events.fromClient('entity-check', (player, data) => {
      if (!(player instanceof kcds.players.Player) || data.id !== player.id || data.public !== 'čau' ||
          data.owner !== player.id || data.secret !== false || data.otherOwner !== false) {
        throw Error('Invalid Entity/SharedState roundtrip');
      }
      if (data.generation !== generation) {
        throw Error('Client resource generation mismatch');
      }
      console.log('ENTITY CHECK round=' + generation + ' player=' + player.id);
      kcds.core.Chat.sendToPlayer(player, 'resource-roundtrip-ok-' + generation);
    });
    """,
        encoding="utf8",
    )
    (resource / "client.cjs").write_text(
        """
    const generation = 1;
    let ids = [];
    let sent = false;
    kcds.core.Events.on('test-ids', payload => {
      ids = payload;
    });
    setInterval(() => {
      const player = kcds.players.LocalPlayer;
      if (sent || !player || ids.length !== 2 || player.state.get('public') !== 'čau' ||
          player.state.get('owner') !== player.id) {
        return;
      }
      let other;
      try {
        other = new kcds.players.Entity(ids.find(id => id !== player.id));
      } catch {
        return;
      }
      if (other.state.get('public') !== 'čau') {
        return;
      }
      sent = true;
      kcds.core.Events.toServer('entity-check', { generation, id: player.id, public: player.state.get('public'),
        owner: player.state.get('owner'), secret: player.state.has('secret'),
        otherOwner: other.state.has('owner') });
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
                    [probe_exe, port, runtime_exe, "reload"],
                    stdout=probe_log,
                    stderr=probe_log,
                    creationflags=flags,
                    env=licence.probe_env(),
                )
                try:
                    reloaded = False
                    deadline = time.monotonic() + 35
                    while probe.poll() is None:
                        log = (work / "server.log").read_text(encoding="utf8")
                        if not reloaded and log.count("ENTITY CHECK round=1 ") >= 2:
                            # Rewrite both compiled entries. State lives on the entity,
                            # so restarting the resource must preserve its owner values.
                            for name in ("server.cjs", "client.cjs"):
                                entry = resource / name
                                text = entry.read_text(encoding="utf8")
                                entry.write_text(
                                    text.replace("const generation = 1;", "const generation = 2;"),
                                    encoding="utf8",
                                )
                            server.stdin.write("ensure entity-check\n")
                            server.stdin.flush()
                            reloaded = True
                        if time.monotonic() > deadline:
                            raise RuntimeError("Entity/SharedState reload roundtrip timed out")
                        time.sleep(0.05)
                    print((work / "probe.log").read_text(encoding="utf8"))
                    if probe.returncode or not reloaded:
                        raise RuntimeError("Entity/SharedState native reload roundtrip failed")
                    log = (work / "server.log").read_text(encoding="utf8")
                    if (
                        log.count("ENTITY CHECK round=1 ") != 2
                        or log.count("ENTITY CHECK round=2 ") != 2
                    ):
                        raise RuntimeError("Duplicate or missing resource-generation callbacks")
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
        raise SystemExit("Usage: entity_resource_roundtrip.py <server> <probe> <runtime> <node>")
    with tempfile.TemporaryDirectory(prefix="kcds-entity-roundtrip-") as folder:
        run(*sys.argv[1:], Path(folder))
