KCDS Server {{VERSION}}
=======================

Requirements
- Linux x64 with glibc 2.36 or newer and libcurl. The server runs on Linux
  and in Docker.
- Node.js 22 or newer on PATH, 26 LTS recommended (https://nodejs.org/).

Start
- Run ./start-server.sh. The server listens on UDP port 7777.
- Players outside your network need UDP 7777 open in the firewall and
  forwarded on your router.
- Type commands into the server's terminal.

Settings: server.json
- Every setting is in server.json next to this file. Change it and start the
  server again. A key you leave out keeps its default:
  - port (7777), bind ("0.0.0.0": every address), level ("trosecko",
    "kutnohorsko" or "klaster", the last a DLC level).
  - name: the name players see in the server browser (at most 64 characters).
  - maxPlayers (32), password (none: anyone with the game may join).
  - master: "off" keeps the server private (not listed; players join by
    address or a kcds://join/ link).
  - publicAddress: "host:port" players dial, when it is not the address the
    master sees (a DNS name, a proxy).
  - image: an https URL of the server's card art in the browser.
  - httpPort / httpBind: resources answer HTTP requests (Http.route) and the
    server its status (/info.json, /dynamic.json, /players.json) on this TCP
    port. publicPlayerList: false keeps /players.json private.
  - adminPort / adminPassword (8 or more characters) / adminBind: the web
    admin in your browser, http://127.0.0.1:<adminPort>/ (this computer only
    unless adminBind says otherwise; adminTlsCert / adminTlsKey for HTTPS).
  - restartTimes: ["06:00", "18:00"] restarts every day at those times, with
    warnings to the players.
  - tickRate, interestRadius, interestHysteresis, snapshotWorkers, metrics:
    tuning, see the documentation.
- An environment variable wins over server.json (KCDS_SERVER_NAME,
  KCDS_SERVER_PASSWORD, ...), a command line option over both
  (./kcd2s-server --port 7778 --level kutnohorsko ...).

Resources
- Put your resources in resources/, each in a folder of its own: every folder
  with a package.json "kcds" section is started. The server comes with none.

Server browser
- The server lists itself in the in-game server browser through the master
  server https://master.kcdsynced.com: name, map, players/max, version and
  address, refreshed every 15 seconds and removed when the server stops.
- A server bound to 127.0.0.1 is never listed unless publicAddress is set.

Resources over HTTP
- Server resources can call web services with fetch and keep data in SQLite
  (node:sqlite) or in a database through their own npm packages.
- Open the HTTP and admin ports in your firewall only if something outside
  this machine needs them.

Every player needs a legally owned Kingdom Come: Deliverance II, on every
server, this one included.
