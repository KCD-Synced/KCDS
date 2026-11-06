# KCDS — multiplayer for Kingdom Come: Deliverance II

KCDS (KCD2-Synced) adds multiplayer to Kingdom Come: Deliverance II. People host their own servers,
players join them from an in-game server browser, and each server defines its gameplay with
**resources**: scripts in JavaScript or TypeScript that use the KCDS SDK.

- Website and player downloads: <https://kcdsynced.com>
- Documentation for players, server owners and resource authors: <https://kcdsynced.com/docs>
  (source: [KCD-Synced/KCDS-DOCS](https://github.com/KCD-Synced/KCDS-DOCS))

KCDS is a fan project. It is not affiliated with or endorsed by Warhorse Studios or Deep Silver.
Kingdom Come: Deliverance is a trademark of its owners. Every player needs their own copy of the game.

## What it does

- **Players in one world:** movement, animations, appearance and equipment, combat (melee, ranged,
  blocks, clinches, knock-outs), riding, carrying, sitting, emotes and voice chat, all synchronised
  between the players' games.
- **A server that decides:** the authoritative server owns the world state: players, NPCs, horses
  and dogs, items and inventories, doors, stashes, props, areas, time and weather.
- **The game's own systems:** inventory and equipment, skills, perks and buffs, crafting, alchemy
  and smithing, gathering, cooking, shops and player trade, dice, quests and conversations, books,
  the map and compass, the HUD and the game's own menus.
- **Resources:** server and client scripts with an SDK of events, shared state, timers, chat,
  web views, the game's UI and much more, hot-reloaded while the server runs.
- **Hosting:** a Linux server package or a Docker image, a join queue, a whitelist, admin accounts,
  a web admin page and automatic server updates.
- **Tools:** World Architect, an in-game level editor whose work a server can load.

## How it fits together

```
 Player's PC                                          Server
 ┌───────────────────────────────────────┐            ┌────────────────────────────────────┐
 │ kcds-launcher.exe  (signed updates)   │            │ kcd2s-server                       │
 │   └─ starts KingdomCome.exe and       │   KSTP     │   world state, replication,        │
 │      injects kcd2s-game-client.dll ───┼── (UDP) ───┼── interest, admission              │
 │        ├─ engine bridge (WHGame.dll)  │            │   └─ resource host (Node.js):      │
 │        ├─ replicas of other players   │            │      the server's resources        │
 │        └─ client resources + web UI   │            └────────────────────────────────────┘
 └───────────────────────────────────────┘
```

- **KSTP** is KCDS's own UDP transport: reliable and unreliable channels, a handshake, and
  bit-packed snapshots and deltas of the replicated world (`Backend/wire`, `Backend/transport`,
  `Backend/replication`).
- The **server** (`Backend/integration`) is authoritative. Each player's game simulates its own
  body and reports it; the server checks the reports and sends every other player what they may see.
- The **resource host** (`Backend/scripting/runtimes/js`) runs a server's resources in Node.js and
  talks to the server over a local channel.
- The **game client** (`Client/runtime`) is a DLL loaded into the game. It finds the game's
  functions by byte signature, hooks them, creates the other players as real game characters
  (replicas) and drives them from the server's snapshots.
- The **launcher** verifies Ed25519-signed releases, updates only changed files, then starts the
  game with the client DLL. It ships with every release but its source is not public; the code it
  shares with the client (JSON, server addresses, crash capture) is in `Client/launcher`. A local
  build starts the game with `kcd2s-launch-client.exe` instead (see *Running it locally*).

## Repository layout

| Folder | Contents |
|---|---|
| `Backend/core`, `wire`, `transport`, `replication` | Headless libraries: KSTP, wire format, replicated world |
| `Backend/integration` | The authoritative server and its gameplay state |
| `Backend/scripting`, `Backend/sdk` | The resource runtime and the SDK it exposes |
| `Backend/resources` | Sample server resources (free roam, dice, crafting, horses) |
| `Backend/voice` | The voice relay (built in, or the standalone `kcds-voice`) |
| `Client/runtime` | The game client DLL, one folder per feature (`combat/`, `riding/`, `inventory/`, ...) |
| `Client/bridge` | Signature scanner and resolver, the engine bridge |
| `Client/launcher` | Code the client shares with the launcher: JSON, server addresses, crash capture |
| `Client/ui`, `Client/menu_native`, `Client/masterclient` | In-game web UI, main menu hooks, server browser |
| `Client/resources` | Client-side built-in resources and their runtime |
| `Client/voice` | The voice client (Opus, noise suppression, spatial audio) |
| `Dev/tools` | Generators of the game-data catalogs, SDK and packaging tools |
| `Dev/testkit` | The C++ test harness and live test helpers |
| `Dev/docker` | Server, voice and Windows cross-build images |
| `Dev/examples` | Example resources |

## Building

### Server, libraries and tests (Linux or Windows)

Requirements: CMake 3.20+, a C++20 compiler (GCC 14, Clang or MSVC 2022), and on Linux
`libcurl4-openssl-dev`.

    cmake --preset dev
    cmake --build build/dev --parallel
    ctest --test-dir build/dev --output-on-failure

The Linux server package and the Docker image are described in `Dev/docker/server/Dockerfile`.

### Game client (Windows x64)

Requirements: Visual Studio 2022 (x64), CMake, Node.js, Python 3.12, and the
[MinHook](https://github.com/TsudaKageyu/minhook) source:

    cmake -S . -B build/client -G "Visual Studio 17 2022" -A x64 ^
      -DKCD2S_BUILD_CLIENT=ON -DKCD2S_BUILD_GAME_CLIENT=ON ^
      -DKCD2S_MINHOOK_DIR=<path to MinHook> -DKCDS_DEV_BUILD=ON
    cmake --build build/client --config Release --parallel

`KCDS_DEV_BUILD=ON` starts the game windowed with `-devmode`, which is handy for testing.

A build from source uses a **development handshake key**. It joins servers built from source, not
the official servers: those are built with a private key.

### Running it locally

    kcd2s-server.exe 7777 127.0.0.1 trosecko
    kcd2s-launch-client.exe <path to KingdomCome.exe> <path to kcd2s-game-client-v4.dll> 64001

Run the second command again with `64002` for a second player on the same PC. Steam must be running
with the game. Logs are written beside the client DLL. See
[`Client/runtime/README.md`](Client/runtime/README.md).

## Testing

| What | How |
|---|---|
| C++ libraries and server | `ctest --test-dir build/dev --output-on-failure` |
| Resource runtime and SDK | `node --test Backend/scripting/runtimes/js/*.test.cjs` |
| Guides' examples | the same, with [KCDS-DOCS](https://github.com/KCD-Synced/KCDS-DOCS) checked out beside this repository (or `KCDS_DOCS_DIR` pointing at it); without it they skip |
| Python tools | the `python -m unittest` lines of the `source-format` job in CI |
| Formatting | `python Dev/tools/format_source.py --check` (clang-format 19.1.5, Prettier, Black 25.9.0) |

CI (`.github/workflows/ci.yml`) runs all of these on every pull request.

## Contributing

Pull requests are welcome: bug fixes, new SDK features, game systems that are not synchronised yet,
tests and documentation (documentation goes to [KCDS-DOCS](https://github.com/KCD-Synced/KCDS-DOCS)).

1. Read [CONTRIBUTING.md](CONTRIBUTING.md): forks are for contributing back, and it states the
   terms of your contribution. Fork the repository and create a branch from `main`.
2. Put new code in the folder of its feature (`Client/runtime/<feature>/`,
   `Backend/scripting/runtimes/js/<feature>/`, ...). Entry points such as `game_client.cpp` only
   wire features together.
3. Add tests: C++ tests under the module's `tests/` folder, resource runtime tests as
   `*.test.cjs` next to the code.
4. Run the tests and `python Dev/tools/format_source.py` (it formats C++, JavaScript and Python).
5. Open a pull request that says what changes in the game and how you tested it.

Comments say what the code does and why, in plain sentences. No issue numbers, dates or names in
comments: the pull request holds that.

### Reverse engineering the game with Ghidra

Most client features need a function of the game: a method to call, or one to hook. KCDS never
uses fixed addresses. Each function is found by a **byte signature**, a pattern of the function's
own instructions that matches exactly one place in `WHGame.dll`. The client is pinned to the game
version in `Client/runtime/README.md`; a signature that matches nowhere or twice stops that feature
and logs its name.

You need your own copy of the game, [Ghidra](https://ghidra-sre.org) (free), and optionally
x64dbg or Cheat Engine to watch the running game.

1. **Load the game.** Create a Ghidra project and import
   `KingdomComeDeliverance2/Bin/Win64MasterMasterSteamPGO/WHGame.dll`. Run the auto-analysis with the
   defaults; the DLL is large, so the first analysis takes a long time. Save the project.
2. **Find your function.** Good entry points:
   - strings: console variables (`wh_...`, `g_...`), log and UI texts, script binding names;
     search them (Search → For Strings) and follow their references;
   - RTTI class names (`C_Player`, `C_Inventory`, ...): Ghidra names many vtables from the
     game's RTTI, and a vtable lists the class's virtual functions;
   - CryEngine's public SDK, which the game's engine is based on, for interfaces such as
     `IEntitySystem` or `IEntity`;
   - existing KCDS code: the function a feature already resolves often calls the one you need.
3. **Make a signature.** Copy the bytes from the start of the function (Listing view → Copy
   Special → Byte String). Replace every byte that changes between builds with `??`: the 4-byte
   offsets of `call`/`jmp` targets (`E8 ?? ?? ?? ??`) and of RIP-relative operands
   (`48 8B 05 ?? ?? ?? ??`). Keep the pattern just long enough to be unique, and check it with
   Search → Memory: exactly one result.
4. **Resolve it in the client.** In your feature's install function:

   ```cpp
   #include "../core/natives.hpp"

   // An example pattern: the name is what the log shows when it does not match exactly once.
   void* fn = resolve_unique("MyFeatureTarget", "48 89 5C 24 08 57 48 83 EC 20 48 8B FA E8 ?? ?? ?? ??");
   ```

   For a global, give the offset of its RIP-relative operand inside the match and the length of
   that instruction: `resolve_unique("Name", pattern, 3, 7)` returns the address the operand points
   at. `slot<Fn>(object, i)` calls a virtual function by its vtable index.
5. **Hook or call it.** Hooks go through MinHook in the client's hook batch; follow an existing
   feature, for example `Client/runtime/action_hints/`, which resolves its natives, hands them to
   its feature code and logs whether it installed.
6. **Test in the game.** Start a local server and two clients (see *Running it locally*) and check
   both games. Log what you resolved and what the hook saw; the logs are beside the client DLL.

Rules:

- Never commit the game's files or anything copied out of them: no DLLs, decompiled code, models,
  textures, sounds or text. Signatures, offsets of fields and your own code are fine. The game-data
  catalogs in `Backend/scripting/runtimes/js` are generated by the scripts in
  `Dev/tools/*/build_*.py` from a game installation.
- Describe the game's behaviour in comments, not addresses.
- No cheats, no tools against other players, and nothing that works around the need to own the game.

## License

KCDS is **source-available**, not open source. The code is licensed under the
[PolyForm Strict License 1.0.0](LICENSE): you may read and use it for noncommercial purposes, but
not distribute it or make changed or new versions of it. [CONTRIBUTING.md](CONTRIBUTING.md) adds
one permission: to fork and change it only to send pull requests back here. There is one official
KCDS, released at <https://kcdsynced.com>.

Resources you write for KCDS are your own work, under terms you choose.

## Third-party code

Bundled with its licence in its folder: Opus, RNNoise and SpeexDSP (BSD), miniaudio (public
domain / MIT-0), Monocypher (CC0 / BSD-2-Clause), Steam Audio (Apache-2.0), wasmoon (MIT), and the
fonts Cinzel and EB Garamond (SIL Open Font License, `Client/runtime/branding/assets/fonts`).
Not bundled: MinHook (BSD-2-Clause), which you point the client build at, and sentry-native with
Crashpad (MIT / Apache-2.0), which the build downloads and checks by SHA-256.
