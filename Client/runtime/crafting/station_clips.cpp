#include "station_clips.hpp"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "../props/prop_entity.hpp"
#include "kcd2s/integration/station_action.hpp"

namespace kcds::runtime::crafting::clips {
namespace {

namespace wire = kcd2s::integration;

Natives natives;

using Get = void*(__fastcall*)(void* object);

template <class T>
T read(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

constexpr std::size_t kEntityCharacter = 101;
constexpr std::size_t kCharacterSkeletonAnim = 5;
constexpr std::size_t kClipCount = 15;
constexpr std::size_t kClipAt = 18;
constexpr std::size_t kClipId = 16;
constexpr std::size_t kClipFlags = 76;
constexpr std::size_t kClipParametric = 8;
constexpr std::size_t kParametricCount = 9;
constexpr std::size_t kParametricValues = 12;
constexpr std::size_t kParametricIds = 28;
constexpr std::size_t kCharacterAnimationSet = 16;
constexpr std::size_t kSetIdOfName = 6;
constexpr std::size_t kSetNameOfId = 7;
// ISkeletonAnim's normalized time of a FIFO clip (slot 28: a blend space's from its
// dominant example's keys; the CAnimation's own field + 68 does not run for one, live).
constexpr std::size_t kClipNormalizedTime = 28;
// ICharacterInstance StartAnimationProcessing (slot 19, + 152) and its parameters as the entity
// slot fills them: the location as a QuatTS (rotation x y z w, position, scale),
// whether it runs for the renderer, the camera distance and a delta time (-1: the frame's).
constexpr std::size_t kStartAnimationProcessing = 19;
struct ProcessParams {
  float rotation[4];
  float position[3];
  float scale;
  bool on_render;
  float distance;
  float override_delta;
};
static_assert(sizeof(ProcessParams) == 44, "the engine's SAnimationProcessParams");

// The newest clip on a skeleton's layer 0, or null.
const std::uint8_t* newest_clip(void* skeleton) {
  using Count = unsigned(__fastcall*)(void* skeleton, unsigned layer);
  using At = const std::uint8_t*(__fastcall*)(void* skeleton, unsigned layer, unsigned index);
  const unsigned count = slot_of<Count>(skeleton, kClipCount)(skeleton, 0);
  if (count == 0 || count > 16) {
    return nullptr;
  }
  return slot_of<At>(skeleton, kClipAt)(skeleton, 0, count - 1);
}

}  // namespace

void install(const Natives& given) {
  natives = given;
}

void* character_of(void* entity) {
  if (!entity) {
    return nullptr;
  }
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  return slot_of<GetCharacter>(entity, kEntityCharacter)(entity, 0);
}

void process(void* entity, float distance) {
  void* character = character_of(entity);
  if (!character) {
    return;
  }
  ProcessParams params{};
  props::entity::world_pose(entity, params.position, params.rotation);
  params.scale = props::entity::world_scale(entity);
  params.on_render = false;
  params.distance = distance;
  params.override_delta = -1.0f;
  using Start = void(__fastcall*)(void* character, const ProcessParams* params);
  slot_of<Start>(character, kStartAnimationProcessing)(character, &params);
}

void* skeleton_of(void* entity) {
  void* character = character_of(entity);
  if (!character) {
    return nullptr;
  }
  return slot_of<Get>(character, kCharacterSkeletonAnim)(character);
}

std::string clip_name(void* character, std::int32_t clip) {
  void* set =
      character && clip >= 0 ? slot_of<Get>(character, kCharacterAnimationSet)(character) : nullptr;
  if (!set) {
    return {};
  }
  using NameOfId = const char*(__fastcall*)(void* set, int clip);
  const char* raw = slot_of<NameOfId>(set, kSetNameOfId)(set, clip);
  std::string name = raw ? std::string(raw) : std::string();
  const std::string blend_space = ".bspace";
  if (name.size() > blend_space.size() &&
      name.compare(name.size() - blend_space.size(), blend_space.size(), blend_space) == 0) {
    name.resize(name.size() - blend_space.size());
  }
  if (!wire::station_action_detail::clip_name_valid(name) || name[0] == '#') {
    name = "#" + std::to_string(clip);
  }
  return name;
}

std::int32_t clip_id(void* character, const std::string& name) {
  if (!name.empty() && name[0] == '#') {
    return std::atoi(name.c_str() + 1);
  }
  void* set = character ? slot_of<Get>(character, kCharacterAnimationSet)(character) : nullptr;
  if (!set) {
    return -1;
  }
  using IdOfName = int(__fastcall*)(void* set, const char* name);
  return slot_of<IdOfName>(set, kSetIdOfName)(set, name.c_str());
}

std::int32_t top_clip(void* skeleton, std::uint32_t& flags) {
  flags = 0;
  if (!skeleton) {
    return -1;
  }
  const std::uint8_t* clip = newest_clip(skeleton);
  if (!clip) {
    return -1;
  }
  flags = read<std::uint32_t>(clip, kClipFlags);
  return read<std::int32_t>(clip, kClipId);
}

float top_clip_time(void* entity) {
  void* skeleton = skeleton_of(entity);
  if (!skeleton) {
    return 0.0f;
  }
  const std::uint8_t* clip = newest_clip(skeleton);
  using TimeOf = float(__fastcall*)(void* skeleton, const std::uint8_t* clip);
  const float time = clip ? slot_of<TimeOf>(skeleton, kClipNormalizedTime)(skeleton, clip) : 0.0f;
  return std::isfinite(time) && time >= 0.0f && time <= 1.0f ? time : 0.0f;
}

// Read under a fault guard: a diagnostic of the owner's clip.
bool clip_parameter(void* skeleton, int id, float& value) {
  __try {
    const std::uint8_t* clip = newest_clip(skeleton);
    const auto* parametric = clip ? read<const std::uint8_t*>(clip, kClipParametric) : nullptr;
    if (!parametric) {
      return false;
    }
    const unsigned parameters = parametric[kParametricCount];
    for (unsigned i = 0; i < parameters && i < 4; ++i) {
      if (parametric[kParametricIds + i] == id) {
        value = read<float>(parametric, kParametricValues + 4 * i);
        return true;
      }
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool start(void* skeleton, int clip, std::uint32_t flags, float blend, float key_time) {
  return skeleton && natives.start_clip &&
         natives.start_clip(skeleton, clip, flags, blend, key_time);
}

void stop(void* skeleton) {
  if (skeleton && natives.stop_clips) {
    natives.stop_clips(skeleton);
  }
}

void clear(void* skeleton) {
  if (skeleton && natives.clear_clips) {
    natives.clear_clips(skeleton);
  }
}

}  // namespace kcds::runtime::crafting::clips
