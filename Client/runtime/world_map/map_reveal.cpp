// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "map_reveal.hpp"

#include <cstdint>
#include <cstring>

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "resource_world_map.hpp"

namespace kcds::game_client {

DWORD resolve_map_reveal_natives() {
  // All patterns are original WHGame bytes; RIP-dependent displacements wildcarded.
  native_reveal_fow = reinterpret_cast<RevealNativeFow>(
      resolve_unique("NativeRevealFow",
                     "40 53 48 83 EC 20 48 8B D9 48 8B 49 68 48 85 C9 74 0A F6 C1 03 75 05 E8 ?? "
                     "?? ?? ?? 48 8B CB 48 C7 43 68 02 00 00 00"));
  native_discover_map = reinterpret_cast<DiscoverNativeMap>(resolve_unique(
      "NativeDiscoverAllMap", "48 8B 0D ?? ?? ?? 03 33 D2 48 8B 89 B8 00 00 00 E9 ?? ?? CF 00"));
  if (!native_reveal_fow || !native_discover_map) {
    log("Required native map discovery signatures missing");
    return 1;
  }
  return 0;
}

namespace {

// MCP player_revealFow: Services.PlayerModule(+296), FOWManager (+56), current I_FogOfWar
// (+24), whose vtable slot 9 lifts the fog; null when the level has none. Its slot 10
// (C_FogOfWar<3,4>) reveals around a point (Vec2 by value) with the fog's own reveal
// brush when its flag is set, as the player's walking does, and tells the fog's listeners.
constexpr std::size_t kRevealAround = 10;

using RevealAround = std::uint64_t(__fastcall*)(void* fow, std::uint64_t point, bool apply);
void* level_fog(void* services) {
  auto player_module =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(services) + 296);
  auto manager = player_module ? *reinterpret_cast<std::uint8_t**>(player_module + 56) : nullptr;
  auto fow = manager ? *reinterpret_cast<void**>(manager + 24) : nullptr;
  if (!fow || (*reinterpret_cast<void***>(fow))[9] != reinterpret_cast<void*>(native_reveal_fow)) {
    return nullptr;
  }
  return fow;
}

}  // namespace

void reveal_server_map(void* services, const kcd2s::integration::WorldConfig& world) {
  // A script cleared the whole level's fog (WorldMap.clearAllFog): the fog only,
  // as player_revealFow does; discovery is left alone.
  if (kcds::runtime::world_map::take_fog_reveal() && native_reveal_fow) {
    if (void* fow = level_fog(services)) {
      engine_stage = "script fog reveal";
      native_reveal_fow(fow);
      log("MAP fog of war cleared by a script");
    }
  }
  // A script cleared the fog around points (WorldMap.clearFog).
  for (const auto& point : kcds::runtime::world_map::take_fog_points()) {
    void* fow = native_reveal_fow ? level_fog(services) : nullptr;
    if (!fow) {
      break;
    }
    std::uint64_t packed = 0;
    std::memcpy(&packed, point.data(), sizeof(packed));
    engine_stage = "script fog reveal around";
    (*reinterpret_cast<RevealAround**>(fow))[kRevealAround](fow, packed, true);
    log("MAP fog of war cleared around %.1f %.1f by a script", point[0], point[1]);
  }
  // DiscoverAll uses the RPG manager. Apply once per loaded server world, never during campaign
  // startup.
  // A server with revealMap off leaves the fog and the discoveries to the game.
  if (revealed_map_revision != world.revision && world.debug && world.reveal_map &&
      native_reveal_fow && native_discover_map) {
    if (void* fow = level_fog(services)) {
      engine_stage = "native reveal server map";
      native_reveal_fow(fow);
      native_discover_map();
      revealed_map_revision = world.revision;
      log("MAP native fog revealed and locations discovered revision=%u", world.revision);
    }
  }
}

}  // namespace kcds::game_client
