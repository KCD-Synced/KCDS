#pragma once
#include <array>
#include <cstdint>
#include <string>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/integration/hands.hpp"

// A stack of items lying in the world: the server's NetworkGroundItem entity
// (local/movement.hpp), which every client in view lays as the game's own stack.

namespace kcd2s::integration {

// NetworkGroundItem's fields, in schema order.
enum GroundItemField : core::FieldId {
  GroundPosition = 0,  // Vec3: where it lies
  GroundRotation,      // Quat
  GroundItemClass,     // Str: the class as scripts write it (hands.hpp item_class_script_text)
  GroundAmount,        // I64: units in the stack, 1 to 10000
  GroundQuality,       // I64: 0 leaves it to the class
  GroundHealth,        // F32: absolute 0 to 1, -1 leaves it to the class
  GroundResting,       // Bool: settled where it stays
  GroundDroppedBy,     // I64: the network id of the player who dropped it, 0 for the server
  GroundScriptId,      // I64: its id in scripts
  GroundDimension,     // I64
  GroundPinned,        // Bool: held where it lies without a body (an arrow stuck in a target)
};

inline constexpr std::uint32_t kMaxGroundAmount = 10000;

}  // namespace kcd2s::integration
