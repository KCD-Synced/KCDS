#include "hand_effects.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"

#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::runtime::crafting::hand_effects {

namespace {

namespace wire = kcd2s::integration;
namespace entity = kcds::runtime::props::entity;

// CProceduralClipParticleEffect: its scope, and in its params (copied to + 40) the effect's
// name, the joint's CRC, the position offset and the rotation offset.
constexpr std::size_t kClipScope = 24;
constexpr std::size_t kParamsEffect = 8;
constexpr std::size_t kParamsJoint = 16;
constexpr std::size_t kParamsPosition = 24;
constexpr std::size_t kParamsRotation = 36;
// IScope's entity id (vtable slot 9).
constexpr std::size_t kScopeEntityId = 9;
// IEntity GetCharacter (slot 101); ICharacterInstance GetIAttachmentManager (slot 9);
// IAttachmentManager CreateAttachment (slot 2: name, type, joint, force) and GetInterfaceByName
// (slot 8); IAttachment HideAttachment (17), AddBinding (26) and ClearBinding (30).
constexpr std::size_t kGetCharacter = 101;
constexpr std::size_t kAttachmentManager = 9;
constexpr std::size_t kCreateAttachment = 2;
constexpr std::size_t kAttachmentByName = 8;
constexpr std::size_t kHideAttachment = 17;
// IAttachment AlignJointAttachment (slot 24): the attachment's default place becomes its joint's.
constexpr std::size_t kAlignJointAttachment = 24;
constexpr std::size_t kAddBinding = 26;
constexpr std::size_t kClearBinding = 30;
constexpr int kJointAttachment = 0;
constexpr std::size_t kEffectAttachmentBytes = 56;
// The hand slots' joints the clips put their effects on.
constexpr char kLeftSlot[] = "LeftHandSlot";
constexpr char kRightSlot[] = "RightHandSlot";
constexpr char kKeyPrefix[] = "#fx";

using ClipEnter = void(__fastcall*)(void* clip, float blend, float duration, const void* params);
using ClipExit = void(__fastcall*)(void* clip, float blend);
using Allocate = void*(__fastcall*)(std::size_t size);
using MakeEffect = void*(__fastcall*)(void* memory, const char* effect, const float* offset,
                                      const float* direction, float scale);

Natives natives;
ClipEnter original_enter = nullptr;
ClipExit original_exit = nullptr;

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

template <class T>
T read(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(T));
  return value;
}

template <class... Args>
void log_line(const char* format, Args... args) {
  if (natives.log) {
    natives.log(format, args...);
  }
}

// CRC32 of a name in lower case, as the game keys joints (SCRCRef, CRC32 lower case).
std::uint32_t crc_lower(const char* text) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char* c = text; *c; ++c) {
    const char lower = (*c >= 'A' && *c <= 'Z') ? char(*c + 32) : *c;
    crc ^= static_cast<std::uint8_t>(lower);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

// The game's rotation from angles about x, y and z (Quat::CreateRotationXYZ): x, y, z, w.
void rotation_xyz(const float angles[3], float out[4]) {
  const float sx = std::sin(angles[0] * 0.5f);
  const float cx = std::cos(angles[0] * 0.5f);
  const float sy = std::sin(angles[1] * 0.5f);
  const float cy = std::cos(angles[1] * 0.5f);
  const float sz = std::sin(angles[2] * 0.5f);
  const float cz = std::cos(angles[2] * 0.5f);
  out[0] = cz * cy * sx - sz * sy * cx;
  out[1] = cz * sy * cx + sz * cy * sx;
  out[2] = sz * cy * cx - cz * sy * sx;
  out[3] = cx * cy * cz + sx * sy * sz;
}

// A clip of the owner's player's body that plays its effect now.
struct Running {
  void* clip = nullptr;
  std::uint32_t entity = 0;
  std::string effect;
  char hand = 0;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
};
std::vector<Running> running;

void forget_clip(void* clip) {
  std::erase_if(running, [clip](const Running& one) { return one.clip == clip; });
}

void __fastcall hooked_enter(void* clip, float blend, float duration, const void* params) {
  original_enter(clip, blend, duration, params);
  void* scope = clip && params ? read<void*>(clip, kClipScope) : nullptr;
  if (!scope) {
    return;
  }
  const char* effect = read<const char*>(params, kParamsEffect);
  const std::uint32_t joint = read<std::uint32_t>(params, kParamsJoint);
  static const std::uint32_t left = crc_lower(kLeftSlot);
  static const std::uint32_t right = crc_lower(kRightSlot);
  const char hand = joint == left ? 'L' : (joint == right ? 'R' : 0);
  if (!effect || !*effect || hand == 0) {
    return;
  }
  using EntityId = std::uint32_t(__fastcall*)(void* scope);
  Running one;
  one.clip = clip;
  one.entity = slot_of<EntityId>(scope, kScopeEntityId)(scope);
  one.effect = effect;
  one.hand = hand;
  float angles[3];
  std::memcpy(one.position.data(), static_cast<const std::uint8_t*>(params) + kParamsPosition,
              sizeof(float) * 3);
  std::memcpy(angles, static_cast<const std::uint8_t*>(params) + kParamsRotation, sizeof(angles));
  rotation_xyz(angles, one.rotation.data());
  forget_clip(clip);
  running.push_back(std::move(one));
}

void __fastcall hooked_exit(void* clip, float blend) {
  forget_clip(clip);
  original_exit(clip, blend);
}

// An effect a replica's body plays: the attachment it is bound on and the effect bound there.
struct Bound {
  void* attachment = nullptr;
  std::string effect;
};
// A replica's body (by its entity id, to know it still exists) and its effects by entry key.
struct Body {
  void* body = nullptr;
  std::uint32_t id = 0;
  std::unordered_map<std::string, Bound> effects;
};
std::unordered_map<std::uint32_t, Body> shown;

// Whether the body the effects were bound on still exists: a body gone took its attachments
// with it.
bool alive(const Body& one) {
  return one.body && entity::by_id(one.id) == one.body;
}

void* attachments_of(void* body) {
  using GetCharacter = void*(__fastcall*)(void* entity, int slot);
  using Get = void*(__fastcall*)(void* object);
  void* character = body ? slot_of<GetCharacter>(body, kGetCharacter)(body, 0) : nullptr;
  return character ? slot_of<Get>(character, kAttachmentManager)(character) : nullptr;
}

void clear(Bound& bound) {
  if (bound.attachment && !bound.effect.empty()) {
    using Clear = void(__fastcall*)(void* attachment, std::uint32_t flags);
    slot_of<Clear>(bound.attachment, kClearBinding)(bound.attachment, 0);
  }
  bound.effect.clear();
}

// Binds the entry's effect on the replica's hand slot joint, as the clip binds it on the owner's.
bool bind(void* body, const std::string& key, const wire::StationEntry& entry, Bound& bound) {
  if (!natives.allocate || !natives.effect_attachment) {
    return false;
  }
  void* manager = attachments_of(body);
  if (!manager) {
    return false;
  }
  using ByName = void*(__fastcall*)(void* manager, const char* name);
  using Create =
      void*(__fastcall*)(void* manager, const char* name, int type, const char* joint, char force);
  const std::string name = "kcds_fx_" + key.substr(1);
  void* attachment = slot_of<ByName>(manager, kAttachmentByName)(manager, name.c_str());
  if (!attachment) {
    const char* joint = entry.hand == 'L' ? kLeftSlot : kRightSlot;
    attachment = slot_of<Create>(manager, kCreateAttachment)(manager, name.c_str(),
                                                             kJointAttachment, joint, 0);
    // Put on its joint's own place, as the clip does with the attachment it clones (without it
    // the attachment, and the stream, stood at the body's origin).
    using Align = void(__fastcall*)(void* attachment);
    if (attachment) {
      slot_of<Align>(attachment, kAlignJointAttachment)(attachment);
    }
  }
  if (!attachment) {
    return false;
  }
  void* memory = reinterpret_cast<Allocate>(natives.allocate)(kEffectAttachmentBytes);
  if (!memory) {
    return false;
  }
  const float up[3] = {0.0f, 1.0f, 0.0f};
  float direction[3];
  kcds::runtime::pose_math::rotate(entry.rotation.data(), up, direction);
  void* effect = reinterpret_cast<MakeEffect>(natives.effect_attachment)(
      memory, entry.model.c_str(), entry.position.data(), direction, 1.0f);
  using AddBinding =
      std::uint32_t(__fastcall*)(void* attachment, void* object, void* skin, std::uint32_t flags);
  using Hide = void(__fastcall*)(void* attachment, std::uint32_t hidden);
  slot_of<AddBinding>(attachment, kAddBinding)(attachment, effect, nullptr, 0);
  slot_of<Hide>(attachment, kHideAttachment)(attachment, 0);
  bound.attachment = attachment;
  bound.effect = entry.model;
  return true;
}

}  // namespace

bool install(const Natives& given) {
  natives = given;
  if (!natives.clip_enter || !natives.clip_exit) {
    log_line("HAND EFFECTS the particle clip's enter or exit is missing");
    return false;
  }
  const bool hooked = MH_CreateHook(natives.clip_enter, reinterpret_cast<void*>(&hooked_enter),
                                    reinterpret_cast<void**>(&original_enter)) == MH_OK &&
                      MH_EnableHook(natives.clip_enter) == MH_OK &&
                      MH_CreateHook(natives.clip_exit, reinterpret_cast<void*>(&hooked_exit),
                                    reinterpret_cast<void**>(&original_exit)) == MH_OK &&
                      MH_EnableHook(natives.clip_exit) == MH_OK;
  log_line("HAND EFFECTS particle clips %s", hooked ? "hooked" : "not hooked");
  return hooked;
}

bool is_effect_key(std::string_view key) {
  return key.rfind(kKeyPrefix, 0) == 0;
}

void add_own(wire::StationAction& action, std::uint32_t local_entity) {
  std::size_t index = 0;
  for (const Running& one : running) {
    if (one.entity != local_entity || action.entries.size() >= wire::kStationEntriesMax ||
        one.effect.size() > wire::kStationModelMax) {
      continue;
    }
    wire::StationEntry entry;
    entry.key = kKeyPrefix + std::to_string(index++);
    entry.model = one.effect;
    entry.placed = true;
    entry.position = one.position;
    entry.rotation = one.rotation;
    entry.hand = one.hand;
    if (wire::station_action_detail::entry_valid(entry)) {
      action.entries.push_back(std::move(entry));
    }
  }
}

void keep(std::uint32_t holder, void* body, const wire::StationAction& action) {
  Body& one = shown[holder];
  if (one.body != body) {
    // A new body: the old one's attachments went with it.
    one = Body{body, entity::id_of(body), {}};
  }
  auto& bodies = one.effects;
  for (const auto& entry : action.entries) {
    if (!is_effect_key(entry.key) || !entry.placed || entry.model.empty() || entry.hand == 0) {
      continue;
    }
    Bound& bound = bodies[entry.key];
    if (bound.effect == entry.model) {
      continue;
    }
    clear(bound);
    if (bind(body, entry.key, entry, bound)) {
      log_line("HAND EFFECTS holder=%u plays %s on the %s hand", holder, entry.model.c_str(),
               entry.hand == 'L' ? "left" : "right");
    }
  }
  for (auto it = bodies.begin(); it != bodies.end();) {
    bool wanted = false;
    for (const auto& entry : action.entries) {
      wanted = wanted || entry.key == it->first;
    }
    if (!wanted) {
      clear(it->second);
      it = bodies.erase(it);
    } else {
      ++it;
    }
  }
}

void forget(std::uint32_t holder) {
  auto found = shown.find(holder);
  if (found == shown.end()) {
    return;
  }
  if (alive(found->second)) {
    for (auto& [key, bound] : found->second.effects) {
      clear(bound);
    }
  }
  shown.erase(found);
}

void reset() {
  shown.clear();
  running.clear();
}

}  // namespace kcds::runtime::crafting::hand_effects
