KCDS Client {{VERSION}}
=======================

Multiplayer for Kingdom Come: Deliverance II (game version 1.5.5).

Install
- The easiest way: KCDS-Setup-{{VERSION}}.exe from the download page installs
  KCDS for your Windows account and adds it to the Start menu.
- By hand, from this zip:
1. Unpack this folder anywhere, e.g. C:\Games\KCDS.
2. The launcher finds a Steam install of the game by itself; otherwise press
   LOCATE GAME FOLDER in its window and pick the game's folder once. To set it
   yourself, open kcds-launcher.json and set:
   - "gameExecutable": the full path to KingdomCome.exe, with doubled
     backslashes, e.g.
     "C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2\Bin\Win64MasterMasterSteamPGO\KingdomCome.exe"
   - "serverAddress": leave it empty to choose a server in the in-game
     server browser, or set a server to join it straight away, e.g.
     "203.0.113.7:7777".
3. Start kcds-launcher.exe. Steam must be running.

Starting KCDS from another launcher
- A community's own launcher can start KCDS straight into its server:
    kcds-launcher.exe --connect play.example.com:7777
  The KCDS launcher updates KCDS first, then starts the game connected to
  that server (it replaces "serverAddress" for that start only). Exit code 0:
  the game started; 3: KCDS is already running; other codes: it could not
  start (see the launcher's log).

Requirements
- Windows 10/11 x64, Kingdom Come: Deliverance II 1.5.5 on Steam,
  Microsoft Edge WebView2 Runtime (part of Windows 11).

Third-party licences
- The licences of the libraries KCDS includes are in the licenses folder, and
  the fonts' licences are in ui\menu\fonts.

This is an early test build: expect bugs, and report them on our Discord.
