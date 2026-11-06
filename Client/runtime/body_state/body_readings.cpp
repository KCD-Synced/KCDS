#include "body_readings.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kcds::runtime::body_state {
namespace {

Natives natives;
Logger log_line = nullptr;

// The actor: its game object and entity.
constexpr std::size_t kActorGameObject = 40;
constexpr std::size_t kActorEntity = 56;
// Vtable slots.
constexpr std::size_t kActorAnimatedHumanSlot = 220;
constexpr std::size_t kAnimatedCharacterControllerSlot = 37;
constexpr std::size_t kControllerContextSlot = 22;
constexpr std::size_t kEntityPhysicsSlot = 79;
constexpr std::size_t kPhysicalStatusSlot = 6;
constexpr std::size_t kGameObjectAspectProfileSlot = 25;
// The animated human's CAnimatedCharacter.
constexpr std::size_t kHumanAnimatedCharacter = 32;
// The animation context: its tag definition and the tag state.
constexpr std::size_t kContextTagDefinition = 8;
constexpr std::size_t kContextTagState = 16;
// The tag state's size (the game clears 20 bytes for one).
constexpr std::size_t kTagStateBytes = 20;
// The tag definition's arrays (CryEngine DynArrays, their count at - 4).
constexpr std::size_t kDefinitionTags = 8;
constexpr std::size_t kDefinitionGroups = 16;
constexpr std::size_t kDefinitionTagValues = 24;
constexpr std::size_t kDefinitionGroupMasks = 32;
constexpr std::size_t kTagEntryBytes = 32;
constexpr std::size_t kTagEntryName = 16;
constexpr std::size_t kGroupEntryBytes = 24;
// The living status (pe_status_living): its type, flying, time flying.
constexpr std::int32_t kLivingStatusType = 2;
constexpr std::size_t kStatusFlying = 4;
constexpr std::size_t kStatusTimeFlying = 8;
// The physics aspect (eEA_Physics).
constexpr std::uint32_t kPhysicsAspect = 8;
// Longer than a stair step lasts: flying shorter than this is not a jump or a fall.
constexpr float kAirborneSeconds = 0.25f;

using Getter = void*(__fastcall*)(void* self);
using GetStatus = int(__fastcall*)(void* physical, void* status);
using GetAspectProfile = std::uint8_t(__fastcall*)(void* game_object, std::uint32_t aspect);

void** vtable(void* object) {
  return *static_cast<void***>(object);
}

template <typename T>
T field(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(T));
  return value;
}

// The object a getter returns, when the object's vtable slot is that getter.
void* call_getter(void* object, std::size_t slot, void* expected) {
  if (!object || vtable(object)[slot] != expected) {
    return nullptr;
  }
  return reinterpret_cast<Getter>(expected)(object);
}

// The game's name hash: CRC-32 of the lower-case name (FindGroup).
constexpr std::uint32_t name_crc(const char* name) {
  constexpr std::uint32_t kReversedPolynomial = 0xEDB88320u;
  std::uint32_t remainder = 0xFFFFFFFFu;
  for (std::size_t index = 0; name[index] != '\0'; ++index) {
    const char letter = name[index];
    const bool upper = letter >= 'A' && letter <= 'Z';
    remainder ^= static_cast<std::uint8_t>(upper ? letter + 32 : letter);
    for (int step = 0; step < 8; ++step) {
      const bool carry = (remainder & 1u) != 0;
      remainder >>= 1;
      if (carry) {
        remainder ^= kReversedPolynomial;
      }
    }
  }
  return remainder ^ 0xFFFFFFFFu;
}

constexpr std::uint32_t kMoveSpeedGroup = name_crc("MoveSpeed");
constexpr std::uint32_t kMoveDirGroup = name_crc("MoveDir");
constexpr std::uint32_t kStanceGroup = name_crc("Stance");
constexpr std::uint32_t kCrouchTag = name_crc("stealth");
constexpr std::uint32_t kSittingTag = name_crc("sitting");

// A DynArray's elements and count, 0 for none or an implausible one.
struct Array {
  const std::uint8_t* data = nullptr;
  std::int32_t count = 0;
};

Array array_at(const void* definition, std::size_t offset) {
  Array array;
  array.data = field<const std::uint8_t*>(definition, offset);
  if (array.data) {
    std::memcpy(&array.count, array.data - 4, sizeof(array.count));
  }
  if (array.count < 0 || array.count > 4096) {
    array.count = 0;
  }
  return array;
}

// The tag a group of the definition holds in the state, kNoTag for none.
std::uint8_t group_tag(const void* definition, const std::uint8_t* state, std::uint32_t group) {
  const Array tags = array_at(definition, kDefinitionTags);
  const Array groups = array_at(definition, kDefinitionGroups);
  const Array values = array_at(definition, kDefinitionTagValues);
  const Array masks = array_at(definition, kDefinitionGroupMasks);
  if (values.count != tags.count || masks.count != groups.count) {
    return kNoTag;
  }
  for (std::int32_t index = 0; index < groups.count; ++index) {
    if (field<std::uint32_t>(groups.data, index * kGroupEntryBytes) != group) {
      continue;
    }
    const std::uint8_t byte = masks.data[2 * index];
    const std::uint8_t mask = masks.data[2 * index + 1];
    if (byte >= kTagStateBytes || (state[byte] & mask) == 0) {
      return kNoTag;
    }
    const std::uint8_t value = state[byte] & mask;
    const std::int32_t last = tags.count < kNoTag ? tags.count : kNoTag;
    for (std::int32_t tag = 0; tag < last; ++tag) {
      if (values.data[2 * tag] == byte && values.data[2 * tag + 1] == value) {
        return std::uint8_t(tag);
      }
    }
    return kNoTag;
  }
  return kNoTag;
}

// The id of the tag named so in the definition, kNoTag for none.
std::uint8_t tag_named(const void* definition, std::uint32_t name) {
  const Array tags = array_at(definition, kDefinitionTags);
  const std::int32_t last = tags.count < kNoTag ? tags.count : kNoTag;
  for (std::int32_t tag = 0; tag < last; ++tag) {
    if (field<std::uint32_t>(tags.data, tag * kTagEntryBytes + kTagEntryName) == name) {
      return std::uint8_t(tag);
    }
  }
  return kNoTag;
}

bool is_tag(const void* definition, std::uint8_t tag, std::uint32_t name) {
  const Array tags = array_at(definition, kDefinitionTags);
  if (tag == kNoTag || tag >= tags.count) {
    return false;
  }
  return field<std::uint32_t>(tags.data, tag * kTagEntryBytes + kTagEntryName) == name;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  const bool complete = natives.animated_human && natives.action_controller && natives.context &&
                        natives.physics && natives.living_status && natives.aspect_profile;
  if (!complete && log_line) {
    log_line("BODY STATE natives missing: tags %u%u%u physics %u%u profile %u",
             unsigned(natives.animated_human != nullptr),
             unsigned(natives.action_controller != nullptr), unsigned(natives.context != nullptr),
             unsigned(natives.physics != nullptr), unsigned(natives.living_status != nullptr),
             unsigned(natives.aspect_profile != nullptr));
  }
  return complete;
}

BodyTags read_tags(void* actor, bool seated) {
  BodyTags tags;
  if (!actor || !natives.animated_human) {
    return tags;
  }
  void* human = call_getter(actor, kActorAnimatedHumanSlot, natives.animated_human);
  void* character = human ? field<void*>(human, kHumanAnimatedCharacter) : nullptr;
  void* controller =
      call_getter(character, kAnimatedCharacterControllerSlot, natives.action_controller);
  void* context = call_getter(controller, kControllerContextSlot, natives.context);
  const void* definition = context ? field<const void*>(context, kContextTagDefinition) : nullptr;
  if (!definition) {
    return tags;
  }
  const auto* state = static_cast<const std::uint8_t*>(context) + kContextTagState;
  tags.move_speed = group_tag(definition, state, kMoveSpeedGroup);
  tags.move_dir = group_tag(definition, state, kMoveDirGroup);
  tags.stance = group_tag(definition, state, kStanceGroup);
  if (seated && tags.stance == kNoTag) {
    tags.stance = tag_named(definition, kSittingTag);
  }
  tags.crouched = is_tag(definition, tags.stance, kCrouchTag);
  return tags;
}

void* action_controller(void* actor) {
  if (!actor || !natives.animated_human) {
    return nullptr;
  }
  void* human = call_getter(actor, kActorAnimatedHumanSlot, natives.animated_human);
  void* character = human ? field<void*>(human, kHumanAnimatedCharacter) : nullptr;
  return call_getter(character, kAnimatedCharacterControllerSlot, natives.action_controller);
}

BodyPhysics read_physics(void* actor) {
  BodyPhysics physics;
  if (!actor || !natives.physics) {
    return physics;
  }
  void* game_object = field<void*>(actor, kActorGameObject);
  if (game_object && vtable(game_object)[kGameObjectAspectProfileSlot] == natives.aspect_profile) {
    physics.profile =
        reinterpret_cast<GetAspectProfile>(natives.aspect_profile)(game_object, kPhysicsAspect);
  }
  void* entity = field<void*>(actor, kActorEntity);
  void* physical = call_getter(entity, kEntityPhysicsSlot, natives.physics);
  if (!physical || vtable(physical)[kPhysicalStatusSlot] != natives.living_status) {
    return physics;
  }
  alignas(16) std::uint8_t status[256] = {};
  std::memcpy(status, &kLivingStatusType, sizeof(kLivingStatusType));
  if (!reinterpret_cast<GetStatus>(natives.living_status)(physical, status)) {
    return physics;
  }
  physics.flying = field<std::int32_t>(status, kStatusFlying) != 0;
  physics.time_flying = field<float>(status, kStatusTimeFlying);
  return physics;
}

bool in_air(const BodyPhysics& physics) {
  return physics.flying && physics.time_flying >= kAirborneSeconds;
}

}  // namespace kcds::runtime::body_state
