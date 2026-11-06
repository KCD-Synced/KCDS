# Main-menu logo (`kcds-branding.pak`)

Our **Synced** variant of KCD2's main-menu logo `Libs/UI/Textures/KCDLogo.dds`, packed into
`kcds-branding.pak`. The pak ships in our own install folder, next to the game-client DLL:

```
<KCDS install>/kcd2s-game-client-v4.dll
<KCDS install>/assets/kcds-branding.pak
```

The DLL mounts it at runtime through the engine's own CryPak, right after the paks of the installed
mods, and only in a game started by our launcher (`Client/runtime/branding/`). **Nothing is written
into the game installation**: a game started from Steam shows the original logo.

## Asset

- `synced-logo.png` — the source artwork (1024×512, transparent). Edit this to change the logo.
- The game asset is **DXT5 (BC3), 1024×512, no mipmaps**. `make_mod.py` keeps the game's original
  128-byte DDS header byte for byte and swaps only the compressed pixels (maximally compatible).
  Because of that header the pak contains game-derived bytes: it is built when a release is
  packaged (`Dev/tools/release/package.py`) and never committed.

## Build

```
python make_mod.py --game "C:/Program Files (x86)/Steam/steamapps/common/KingdomComeDeliverance2"
```

Writes `build/assets/kcds-branding.pak` (or `--out <file>`). The game folder is only read; the
script refuses an output path inside it. Requires Pillow (`pip install pillow`). For a development
kit, copy the pak to `assets/` next to the DLL. `kcd.log` then shows
`Pak '...\assets\kcds-branding.pak' is opened, root: 'data\'` and the DLL log
`BRANDING pak mounted`. Without the pak the DLL logs `BRANDING pak missing` and the game keeps the
original logo.

Older development builds installed this logo as `Mods/KCD2Synced` in the game folder. The launcher
reports that folder if it is still there; delete it.
