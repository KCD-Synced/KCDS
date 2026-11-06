# Native main-menu hooks (`kcds_menu_native`)

Static library linked into the game client DLL. It owns the game's `AddButton` detour and the
main-menu builder hook:

- While the in-game server browser is shown, the native main menu is built **without its buttons**
  (only the logo and the castle scene remain), so our HTML panel is the only menu.
- A button filter drops single buttons by `E_ButtonId`; the multiplayer world uses it to hide
  save/load (11, 12, 19, 20) in the pause menu.

## Why a hook and not a Flash edit

`Libs/UI/Menu.gfx` does not contain the buttons. The game builds them at runtime: the root menu
builder invokes `ShowRootMenu` and then `AddButton` once per entry, and each `AddButton` pushes an
`AddBasicButton` into the Scaleform movie. A re-entrancy guard is raised while the builder runs; a
dropped button releases its by-value callback and tooltip through the game's own cleanup functions.

## Targets (WHGame.dll 1.5.5, resolved by unique signature)

| target | role |
|---|---|
| root menu builder, state 1 | the main menu on a normal start: `ShowRootMenu`, then the entries |
| `AddButton` | adds one button (`AddBasicButton`) |
| callback cleanup | releases the button's callback temporary |
| string cleanup | releases the button's tooltip temporary |

The menu builder of state 2 is the in-game pause menu (it adds Save, Save & Exit), so it is
deliberately not guarded.

Intro skipping and the "Synced" build text are not done here: `Client/runtime/branding` sets them
in memory. Nothing is written into the game installation.
