# Client engine bridge (`Client/bridge/`)

Attaches the replication mirror to the live **WHGame.dll** (CryEngine 5.x "WH" fork) inside the
running game.

## What builds where

| Piece | File | Built | Tested |
|---|---|---|---|
| Signature scanner | `sigscan.{hpp,cpp}` | everywhere (headless) | ✅ `bridge.sigscan` |
| Symbol resolver (fn + RIP-relative global) | `resolver.hpp` | everywhere | ✅ `bridge.resolver` |
| Snapshot → transform extraction | `apply.{hpp,cpp}` | everywhere | ✅ `bridge.apply` |
| **Windows/CryEngine glue** | `engine_win.{hpp,cpp}` | **Windows client build only** | ✗ (needs the game) |

The portable core (scan/resolve/extract) is fully unit-tested on the headless host. Only the raw
engine calls live in `engine_win.cpp`, gated by CMake (`KCD2S_BUILD_CLIENT AND WIN32`) and
`#if defined(_WIN32)`, so the headless CI never touches it.

## How the hook works

1. **Locate** `WHGame.dll` in the injected process (base + image size).
2. **Resolve** engine entry points by byte signature (survives per-patch address shifts):
   `gEnv` (RIP-relative global), `IEntitySystem::GetEntity`, `CEntity::SetWorldTM`. The patterns
   are inlined in `engine_win.cpp`.
3. **Acquire `gEnv`** (`SSystemGlobalEnvironment`) and read `pEntitySystem` from it
   (`ISystem::GetGlobalEnvironment`).
4. **Hook `IDXGISwapChain::Present`** (vtable slot 8, read from a throwaway device/swapchain) via
   **MinHook**. The engine's DX11/DX12 renderer is compiled into `WHGame.dll`, so Present is the
   reliable per-frame tick.
5. **Each frame**: `extract_transforms(mirror)` → for each bound entity, `GetEntity(engineId)` →
   `SetWorldTM(Matrix34)`, then call the original Present.

Entity binding (`bind_entity`/`unbind_entity`) maps our dense `EntityId` to the engine entity id and
is driven by snapshot CONSTRUCT/DESTRUCT.

Remote players are real `C_Player` actors created without the local-only ClientActor setup
(authority, view and action map), so the game keeps exactly one local actor.
