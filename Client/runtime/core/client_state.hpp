// The client's shared state not yet declared in client_internal.hpp: natives found at startup
// (parameter and string helpers, the 3D engine, the map's fog and discovery), the network socket
// layer and the world load's counter.
#pragma once

#include <cstdint>
#include <memory>

#include "../../../Backend/integration/local/runtime.hpp"

namespace kcds::game_client {

// SEntitySpawnParams' constructor and CryString assignment (natives found at startup).
using ParamsInit = void*(__fastcall*)(void*);
using StringAssign = void*(__fastcall*)(void*, const char*);
extern ParamsInit params_init;
extern StringAssign string_assign;
// CCryAction::Update, found at startup and hooked (core/engine_loop.hpp).
extern void* update_target;
extern std::unique_ptr<kcd2s::local::Winsock> winsock;
// The 3D engine global (I3DEngine*), read from the script binding's SetScreenFx.
extern void* const* engine_3d_slot;
// The map's fog reveal and discovery (natives), and the revision the map was revealed for.
using RevealNativeFow = std::intptr_t(__fastcall*)(void*);
using DiscoverNativeMap = std::intptr_t(__fastcall*)();
extern RevealNativeFow native_reveal_fow;
extern DiscoverNativeMap native_discover_map;
extern std::uint32_t revealed_map_revision;
// The level system's load counter (+ 96) when the server's world was requested.
extern std::uint32_t load_counter_before;

}  // namespace kcds::game_client
