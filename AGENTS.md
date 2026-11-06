# KCDS: instructions for AI coding assistants

This file is for assistants (Codex and others that read `AGENTS.md`; Claude Code reads the same text
from `CLAUDE.md`) helping someone contribute to KCDS itself: the mod, the server, the SDK and their
tools. To write a resource for a KCDS server instead, use the CLAUDE.md and CODEX.md of the docs:
<https://kcdsynced.com/docs/claude> and <https://kcdsynced.com/docs/codex>.

When this file and the code disagree, the code is right. When it and README.md or CONTRIBUTING.md
disagree, those are right.

## What KCDS is

KCDS adds multiplayer to Kingdom Come: Deliverance II. It is a platform: a game client (a DLL
loaded into the game), a dedicated server (Linux), its own network protocol on UDP with snapshot and
delta replication, and a resource SDK in JavaScript, TypeScript, Lua and C# for server owners. Read
README.md first: it explains the layout, how to build and how the parts fit together.

- `Backend/`: the server, the shared wire format and replication, and the resource runtimes
  (`Backend/scripting/runtimes/js/`, one folder per feature).
- `Client/`: the game client (`Client/runtime/`, one folder per feature), the engine bridge, voice,
  the resource host in the game and the code the launcher shares (`Client/launcher/`; the launcher
  itself is not in this repository).
- `Dev/`: tools, Docker builds, release tooling and the test kit.
- The user documentation is its own repository, KCD-Synced/KCDS-DOCS. Release notes are on the
  repository's Releases tab (the message of each annotated v tag).

## Rules

1. **Our own code only.** Write KCDS's own code; never commit code, assets or binary dumps that are
   not ours or that come from the game. Third-party libraries already vendored are listed in
   README.md; adding one needs the maintainer's agreement.
2. **The game is called natively.** The client calls or hooks the game's own functions, found by
   byte signatures (`resolve_unique`, see "Reverse engineering the game with Ghidra" in README.md).
   Never fixed addresses, never the game's Lua from the mod. Understand what the game does from the
   game itself before writing a fix; do not guess from logs.
3. **The server decides.** Anything that matters (health, items, money, permissions) is checked and
   changed on the server; a client asks.
4. **Scale.** The server is built for many players: no unbounded queues, no full-state broadcasts
   where a change will do, no blocking network work on the simulation thread.
5. **Feature folders.** New code goes in the folder of its feature; entry points only wire features
   together. A folder that grows past a handful of files is split by sub-feature.
6. **Readable code.** Explicit blocks, useful names, readable line breaks; no dense one-line
   functions, loops or conditionals. Format with `python Dev/tools/format_source.py` (pinned
   clang-format 19.1.5, Prettier 2.8.8, Black 25.9.0) and check with `--check`.
7. **Comments** say what the code does and why, in plain sentences. No issue numbers, dates, names
   or addresses in comments; the pull request holds the history.
8. **Tests with every change.** C++ tests in the module's `tests/` folder (`kcd2s_add_test`),
   resource runtime tests as `*.test.cjs` next to the code. Run them before you say a change works.
9. **Docs with every public change.** A new or changed SDK member gets its description, parameters,
   errors, events and a working example (`Dev/tools/web/sdk-docs/`); guide text goes to
   KCD-Synced/KCDS-DOCS. Every example type-checks (`Dev/tools/web/typecheck_examples.cjs`).
10. **Names.** The product, manifest and runtime names are KCDS / `kcds` (resources use
    `package.json` `kcds`). Existing `kcd2s` C++ namespaces stay as they are.
11. **No secrets.** Never commit keys, tokens, server addresses of people or anything from a
    player's machine. Release secrets live only in the repository's GitHub environments.

## Building and testing

- Server, libraries and tests: `cmake -S . -B build -DKCD2S_BUILD_TESTS=ON
  -DKCD2S_BUILD_GAME_CLIENT=OFF`, `cmake --build build`, `ctest --test-dir build`.
- Resource runtimes: `node --test Backend/scripting/runtimes/js/*.test.cjs` (the guide tests need
  the docs repository next to this one, `../KCDS-DOCS`, or `KCDS_DOCS_DIR`).
- The game client builds on Windows x64 (README.md, "Game client"); release packages build in
  Docker (`Dev/docker/`).

## Pull requests

Work on a branch of a fork, one topic per pull request. Fill in the pull request template: what
changes, how it was tested, and the checklist (every box must be ticked for the checks to pass).
Only the maintainer merges into `main`, and releases are made from `main` by the release workflow.
