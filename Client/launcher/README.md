# What the client shares with the launcher (`kcds::launcher`)

The KCDS launcher (the updater window, the signed update feed, the installer and the self-update
helper) ships with every release, but its source is not public. This folder holds the part the game
client, the server browser and the server share with it:

- `json.hpp` / `lib/json.cpp`: a small bounded JSON reader;
- `server_address.hpp`, `join_link.hpp`: the host:port rule and `kcds://` join links;
- `origin.hpp` / `lib/origin.cpp`: the one rule for an https origin, used by the crash-reporting DSN
  check (and the launcher's update feed);
- `crash*.hpp`, `crash/`: crash-reporting settings, the hand-off between the launcher and the game
  client, log attachments, and the native crash capture with sentry-native
  (`kcds::launcher_sentry`, `crash/sentry_native.cmake`, Windows only);
- `config.example.json`: the launcher's configuration template, which the release packaging fills in.

Official builds check the launcher out into `Client/launcher/private/` (git-ignored); its
`CMakeLists.txt` then adds the launcher's targets next to these. Without it everything else builds,
and a local build starts the game with `kcd2s-launch-client.exe` (see the main README, *Running it
locally*).
