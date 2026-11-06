// Particle effects a client script plays in this game only: the client's Vfx
// (Client/resources/vfx.cjs). Its orders are the server's VfxOrder kinds, so the engine thread
// applies them as it applies the server's (placed_effects.hpp); their effects carry ids from
// kLocalEffectIds up, which the server never gives.
//
//   { op: 'vfxLocal', kind: 'spawn', id, effect, position, rotation, prime, ...tuning }
//   { op: 'vfxLocal', kind: 'destroy', id }
//   { op: 'vfxLocal', kind: 'burst', effect, position, rotation, durationMs, prime, ...tuning }
//   { op: 'vfxLocal', kind: 'burstFrom', effect, entity, offset, rotation, durationMs, prime,
//     ...tuning }
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/placed_objects.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::vfx::local {

inline constexpr std::uint32_t kLocalEffectIds = 0xF0000000u;

// A script's vfxLocal op (any thread): true when the message was one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// The orders the scripts gave since the last call, in order (engine thread).
std::vector<kcd2s::integration::VfxOrder> take_orders();

}  // namespace kcds::runtime::vfx::local
