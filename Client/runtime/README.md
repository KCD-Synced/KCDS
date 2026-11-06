# Game client runtime

The DLL the launcher injects into the game. It is pinned to WHGame.dll 1.5.5: engine functions
resolve through unique byte signatures, and a mismatch stops initialization.

Remote players are created by the native Player/C_Player factory. A per-instance remote virtual
table keeps Player RTTI and factory allocation, while shared Actor lifecycle methods avoid local
input/UI singleton initialization. The local player's movement and combat input selectors, the
NPCManager view context and the StealthWatcher AIObject selector are kept across remote creation
and removal. Remote-created AIObjects cancel their native transform subscriptions before their
native destructor destroys the provider.

Movement snapshots carry position, rotation, velocity, native movement requests and 17 skeleton
motion parameters with a validity mask, plus bounded native animation layer state (clip, native
parameters, sequence). Engine calls run on the framework update thread; socket work runs separately.

## Build

Options: `KCD2S_BUILD_SERVER=ON`, `KCD2S_BUILD_GAME_CLIENT=ON`,
`KCD2S_MINHOOK_DIR=<local MinHook SDK>`.
Targets: `kcd2s-server`, `kcd2s-network-probe`, `kcd2s-game-client`, `kcd2s-launch-client`.

## Local test run

Start `kcd2s-server.exe 7777 127.0.0.1 trosecko` (the Windows server binary serves local tests only;
hosted servers run the Linux server package or the Docker image). Then run

    kcd2s-launch-client.exe <KingdomCome.exe> <kcd2s-game-client.dll> 64001

and the same command with 64002. Steam must be running with the licensed game. The launcher creates
and injects only its own new suspended child process.

The launcher reads the user's persisted `eula_confirmed_version` from
`Saved Games/kingdomcome2/profiles/default/attributes.xml` and restores only that version at
startup; it never raises it or accepts terms. The world starts loading only once the confirmed
version meets the required one, so it never loads behind an unconfirmed EULA overlay.

Client logs (one per PID) are written beside the injected DLL. Do not unload it live. Use a
separate server port for the synthetic network probe; its avatars are diagnostics.
