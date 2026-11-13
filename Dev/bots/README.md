# @kcds/bots

Load-test a KCDS server with bots: hundreds of headless players that join, walk, talk, fight,
shoot, ride and leave like real ones, from one machine or several. The same `kcds-bots` command
runs on macOS, Windows and Linux; npm installs the bot engine built for your machine (a native,
multi-threaded client, about 50 bots a thread).

```sh
npm install -g @kcds/bots
kcds-bots --server 192.168.1.20:7790 --scenario smoke --out smoke.json \
          --metrics-url http://192.168.1.20:7791/metrics
kcds-bots report smoke.json --out report.md
```

## The server

Bots join only a **development** server (built without the release build secret) that runs with
`KCDS_ALLOW_BOTS=1`. A published KCDS server never lets a bot in. Never set `KCDS_ALLOW_BOTS` on
a public server.

For the server's numbers, also set `KCDS_SERVER_METRICS=5` (an interval in seconds) and
`KCDS_HTTP_PORT` (where `/metrics` is served), and enough places: `KCDS_SERVER_MAX_PEERS=400`.
Riding bots need the support resource: copy `resource/kcds-bots` of this package into the
server's `resources` folder.

## Scenarios

`kcds-bots scenarios` lists the ones that come with it:

| Scenario     |                        Bots | What it exercises                                |
| ------------ | --------------------------: | ------------------------------------------------ |
| `smoke`      |       20, every kind, 2 min | a quick check (CI)                               |
| `town`       | 150 wandering in one square | interest management, bandwidth in a crowd        |
| `battle`     |   2 × 50 melee + 20 archers | combat traffic, the melee reach check, tick time |
| `cavalry`    |       60 riders at a gallop | fast entities, corrections, snapshot rate        |
| `join_storm` |     300 joining within 30 s | handshake, queue, admission, first world state   |
| `soak`       |      100 of every kind, 4 h | leaks and tick-time drift                        |

A scenario is a YAML file; `--scenario` takes a name above or a path. See the KCDS docs,
_kcds-bots_, for writing one (groups, behaviours and their settings, areas, network conditions).

## Several machines

Each machine runs its part with `--share I/N`; one of them reads `/metrics`:

```sh
# machine 1 (reads the server's numbers)
kcds-bots --server 192.168.1.20:7790 --scenario battle --share 1/2 --out battle-1.json \
          --metrics-url http://192.168.1.20:7791/metrics
# machine 2
kcds-bots --server 192.168.1.20:7790 --scenario battle --share 2/2 --out battle-2.json
# then, anywhere
kcds-bots merge battle-1.json battle-2.json --out battle.json
kcds-bots report battle.json --out report.md
```

`--workers N` splits one machine's bots over N engines (each multi-threaded); `--threads N` sets
the threads of each engine.

## Results

`result.json` (schema 1) holds the server's numbers (tick time p50/p99/max, missed ticks, CPU and
memory of the server and its resource host, egress, refused checks), the bots' numbers (kbps in and
out per bot, snapshot age, update intervals by distance, corrections a minute, connect-to-spawn
time, failed connects and why) and the bot machines' own load (overruns mean the bots, not the
server, were the limit). A CSV next to it has a row every 5 seconds.

`kcds-bots compare --baseline main.json run-1.json run-2.json` fails when the better run is more
than 10 % worse on tick p99, kbps per player, snapshot age or corrections, or when any check was
refused or a connect failed.

## Building the engine yourself

From the KCDS repository: `cmake -S . -B build -DKCD2S_BUILD_GAME_CLIENT=OFF` and
`cmake --build build --target kcds-bots`; this package finds it in `build/`, or name it with
`--engine <path>`.
