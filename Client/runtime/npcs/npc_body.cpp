#include "npc_body.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "../appearance/components.hpp"
#include "../appearance/female_skeleton.hpp"
#include "npc_suppression.hpp"

namespace kcds::runtime::npcs {
namespace {

// IEntitySystem: the class registry (slot 11, FindClass + 24), SpawnEntity (slot 12) and
// InitEntity (slot 13).
constexpr std::size_t kClassRegistry = 88;
constexpr std::size_t kFindClass = 24;
constexpr std::size_t kSpawnEntity = 96;
constexpr std::size_t kInitEntity = 104;
// SEntitySpawnParams, as CScriptBind_System::SpawnEntity fills it.
constexpr std::size_t kSpawnParamsSize = 144;
constexpr std::size_t kSpawnClass = 24;
constexpr std::size_t kSpawnName = 56;
constexpr std::size_t kSpawnFlags = 64;
constexpr std::size_t kSpawnFlagsExtended = 68;
constexpr std::size_t kSpawnPosition = 80;
constexpr std::size_t kSpawnRotation = 92;
constexpr std::uint32_t kSpawnScriptFlags = 0x1000000;
constexpr std::uint32_t kSpawnScriptFlagsExtended = 0x400000;
// The entity's script table: GetProxy(the script proxy) and the proxy's GetScriptTable.
constexpr std::size_t kGetProxySlot = 74;
constexpr int kScriptProxy = 2;
constexpr std::size_t kScriptTableSlot = 18;
// IScriptTable and ScriptAnyValue.
constexpr std::size_t kReleaseSlot = 4;
constexpr std::size_t kSetValueAnySlot = 7;
constexpr std::size_t kGetValueAnySlot = 8;
constexpr int kAnyBoolean = 2;
constexpr int kAnyString = 5;
constexpr int kAnyTable = 6;
constexpr std::size_t kAnyValue = 8;
constexpr std::size_t kAnySize = 32;

using Get = void*(__fastcall*)(void*);
using FindClass = void*(__fastcall*)(void* registry, const char* name);
using SpawnParamsInit = void*(__fastcall*)(void* params);
using SpawnEntity = void*(__fastcall*)(void* system, void* params, bool init);
using InitEntity = bool(__fastcall*)(void* system, void* entity, void* params);
using GetProxy = void*(__fastcall*)(void* entity, int type);
using Release = void(__fastcall*)(void* table);
using SetValueAny = void(__fastcall*)(void* table, const char* key, const void* value, bool chain);
using GetValueAny = bool(__fastcall*)(void* table, const char* key, void* value, bool chain);

BodyNatives natives;

template <class Function>
Function slot(void* object, std::size_t index) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}

template <class Function>
Function at_offset(void* object, std::size_t offset) {
  return slot<Function>(object, offset / sizeof(void*));
}

void* script_table_of(void* entity) {
  void* proxy = slot<GetProxy>(entity, kGetProxySlot)(entity, kScriptProxy);
  return proxy ? slot<Get>(proxy, kScriptTableSlot)(proxy) : nullptr;
}

// Sets a text value in a script table (the script system copies the text).
void set_text(void* table, const char* key, const char* text) {
  alignas(16) std::uint8_t value[kAnySize] = {};
  std::memcpy(value, &kAnyString, sizeof(kAnyString));
  std::memcpy(value + kAnyValue, &text, sizeof(text));
  slot<SetValueAny>(table, kSetValueAnySlot)(table, key, value, false);
}

// Sets text values in the entity script's Properties table, where the entity's init reads them.
bool set_properties(void* entity,
                    std::initializer_list<std::pair<const char*, const char*>> values) {
  void* script = script_table_of(entity);
  if (!script) {
    return false;
  }
  alignas(16) std::uint8_t properties[kAnySize] = {};
  if (!slot<GetValueAny>(script, kGetValueAnySlot)(script, "Properties", properties, false)) {
    return false;
  }
  int type = 0;
  void* table = nullptr;
  std::memcpy(&type, properties, sizeof(type));
  std::memcpy(&table, properties + kAnyValue, sizeof(table));
  if (type != kAnyTable || !table) {
    return false;
  }
  for (const auto& [key, text] : values) {
    set_text(table, key, text);
  }
  // The table the value held is the script system's; it was added a reference for us.
  slot<Release>(table, kReleaseSlot)(table);
  return true;
}

// A horse's script reads its soul from sharedSoulGuid (Scripts/Entities/.../Horse.lua), a person's
// from guidSharedSoulId (BasicAITable.lua): without it a person is the class's generic soul, with
// no faction or social class (Lua probe: a server NPC had neither, so the game's hit
// reaction fled).
bool set_soul(void* entity, const std::string& soul) {
  return set_properties(entity,
                        {{"sharedSoulGuid", soul.c_str()}, {"guidSharedSoulId", soul.c_str()}});
}

// The game's NPC without AI is a man (Scripts/Entities/AI/NPC_NAI.lua); a woman's replica is
// that class with NPC_Female's model, clothing, animation database and controller, set on the
// entity's own script tables before its init reads them.
bool make_female(void* entity) {
  void* script = script_table_of(entity);
  if (!script) {
    return false;
  }
  set_text(script, "ActionController", "Animations/Mannequin/ADB/wh_female_controllerdefs.xml");
  set_text(script, "AnimDatabase3P", "Animations/Mannequin/ADB/wh_female_database.adb");
  // Without a soul of its own the body is the class's generic soul: a woman's.
  set_text(script, "defaultSoulArchetype", "NPC_Female");
  set_text(script, "OpponentMnTag", "relatedFemale");
  set_text(script, "CombatOpponentMnTag", "oppFemale");
  return set_properties(entity,
                        {{"fileModel", "Objects/Characters/humans/female/skeleton/female.cdf"},
                         {"esClothingConfig", "female2"}});
}

// The body's character and its parts (player_animation.cpp's chain).
constexpr std::size_t kGetCharacterSlot = 101;
constexpr std::size_t kSkeletonAnimSlot = 5;
constexpr std::size_t kAnimationSetSlot = 16;
constexpr std::size_t kAnimIdByNameSlot = 6;
constexpr std::size_t kStartAnimationSlot = 9;
constexpr std::size_t kStopLayerSlot = 10;
// ISkeletonAnim: the clips on a layer (slot 15 their count, slot 18 one of them, a CAnimation with
// the clip id at + 16) and a clip's normalized time (slot 28), as crafting/station_clips reads
// layer 0.
constexpr std::size_t kClipCountSlot = 15;
constexpr std::size_t kClipAtSlot = 18;
constexpr std::size_t kClipTimeSlot = 28;
constexpr std::size_t kClipIdOffset = 16;
constexpr std::uint32_t kClipLayer = 12;
constexpr float kClipBlendIn = 0.2f;
constexpr float kClipBlendOut = 0.25f;
constexpr std::uint32_t kClipLoop = 2;
constexpr std::uint32_t kClipHoldLastKey = 4;
// CA_ALLOW_ANIM_RESTART: the same clip starts again while it is still on the layer (a loop asked
// for after its one-shot, held at its last key, was refused without it).
constexpr std::uint32_t kClipAllowRestart = 0x100;

// CryCharAnimationParams, as player_animation.cpp fills it.
struct ClipParams {
  float transition;
  float key_time;
  float speed;
  float weight;
  std::uint32_t layer;
  float layer_weight;
  std::uint32_t flags;
  std::uint32_t user_token;
  float user_data[8];
};
static_assert(sizeof(ClipParams) == 64);

using GetCharacter = void*(__fastcall*)(void*, int);
using AnimIdByName = int(__fastcall*)(void*, const char*);
using StartAnimation = bool(__fastcall*)(void*, int, const ClipParams*);
using StopLayer = bool(__fastcall*)(void*, std::uint32_t, float);

// The body's skeleton and, when asked, its animation set; null when it has no character.
void* skeleton_of(void* entity, void** animations) {
  void* character = entity ? slot<GetCharacter>(entity, kGetCharacterSlot)(entity, 0) : nullptr;
  if (!character) {
    return nullptr;
  }
  if (animations) {
    *animations = slot<Get>(character, kAnimationSetSlot)(character);
  }
  return slot<Get>(character, kSkeletonAnimSlot)(character);
}

// The clip's name in the animation set: the file's name without its folder and extension.
std::string clip_name(const std::string& clip) {
  std::string name = clip;
  const std::size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) {
    name = name.substr(slash + 1);
  }
  const std::size_t dot = name.rfind('.');
  if (dot != std::string::npos) {
    name.resize(dot);
  }
  return name;
}

}  // namespace

bool play_clip(void* entity, const std::string& clip, bool loop) {
  void* animations = nullptr;
  void* skeleton = skeleton_of(entity, &animations);
  if (!skeleton || !animations) {
    return false;
  }
  const std::string name = clip_name(clip);
  const int id = slot<AnimIdByName>(animations, kAnimIdByNameSlot)(animations, name.c_str());
  if (id < 0) {
    return false;
  }
  ClipParams params{};
  params.transition = kClipBlendIn;
  params.key_time = -1.0f;
  params.speed = 1.0f;
  params.weight = 1.0f;
  params.layer = kClipLayer;
  params.layer_weight = 1.0f;
  params.flags = (loop ? kClipLoop : kClipHoldLastKey) | kClipAllowRestart;
  return slot<StartAnimation>(skeleton, kStartAnimationSlot)(skeleton, id, &params);
}

void stop_clip(void* entity) {
  void* skeleton = skeleton_of(entity, nullptr);
  if (skeleton) {
    slot<StopLayer>(skeleton, kStopLayerSlot)(skeleton, kClipLayer, kClipBlendOut);
  }
}

bool layer_clip(void* entity, std::int32_t& clip, float& time) {
  using Count = unsigned(__fastcall*)(void* skeleton, std::uint32_t layer);
  using At = const std::uint8_t*(__fastcall*)(void* skeleton, std::uint32_t layer, unsigned index);
  using TimeOf = float(__fastcall*)(void* skeleton, const std::uint8_t* animation);
  void* skeleton = skeleton_of(entity, nullptr);
  if (!skeleton) {
    return false;
  }
  const unsigned count = slot<Count>(skeleton, kClipCountSlot)(skeleton, kClipLayer);
  if (count == 0 || count > 16) {
    return false;
  }
  const std::uint8_t* animation = slot<At>(skeleton, kClipAtSlot)(skeleton, kClipLayer, count - 1);
  if (!animation) {
    return false;
  }
  std::memcpy(&clip, animation + kClipIdOffset, sizeof(clip));
  time = slot<TimeOf>(skeleton, kClipTimeSlot)(skeleton, animation);
  return true;
}

bool fragment_field_of(const std::string& field, FragmentField& request) {
  const std::size_t first = field.find('|');
  const std::size_t second = first == std::string::npos ? first : field.find('|', first + 1);
  if (second == std::string::npos || field.compare(second + 1, 1, "|") != 0) {
    return false;
  }
  const std::size_t begin = second + 2;
  const std::size_t bar = field.find('|', begin);
  if (bar == std::string::npos || bar == begin) {
    return false;
  }
  request.sequence = std::uint32_t(std::strtoul(field.c_str(), nullptr, 10));
  request.loop = field.compare(first + 1, second - first - 1, "1") == 0;
  request.fragment = field.substr(begin, bar - begin);
  request.tags = field.substr(bar + 1);
  return true;
}

bool apply_animation_field(void* entity, const std::string& field) {
  if (field.empty()) {
    stop_clip(entity);
    return true;
  }
  const std::size_t first = field.find('|');
  const std::size_t second = first == std::string::npos ? first : field.find('|', first + 1);
  if (second == std::string::npos) {
    return false;
  }
  const bool loop = field.compare(first + 1, second - first - 1, "1") == 0;
  return play_clip(entity, field.substr(second + 1), loop);
}

bool apply_appearance_field(void* actor, const std::string& field) {
  std::string names[4];
  std::size_t part = 0;
  for (const char c : field) {
    if (c != '|') {
      names[part].push_back(c);
    } else if (++part == 4) {
      return false;
    }
  }
  if (part != 3 || !actor) {
    return false;
  }
  kcd2s::integration::AppearanceRequest request;
  request.body = names[0];
  request.head = names[1];
  request.hair = names[2];
  request.beard = names[3];
  return appearance::components::put_on_request(actor, request);
}

void install_bodies(const BodyNatives& given) {
  natives = given;
}

bool set_player_model(void* entity, const char* model) {
  return set_properties(entity, {{"fileModel", model}, {"clientFileModel", model}});
}

bool set_female_model(void* entity) {
  constexpr const char* kFemaleModel = appearance::female_skeleton::kPlayerModel;
  return set_properties(entity, {{"fileModel", kFemaleModel},
                                 {"clientFileModel", kFemaleModel},
                                 {"esClothingConfig", "female2"}});
}

bool init_entity(void* system, void* entity, void* params) {
  return system && entity && at_offset<InitEntity>(system, kInitEntity)(system, entity, params);
}

void* spawn_body(const std::string& name, const char* entity_class, const std::string& soul,
                 const float position[3], const float rotation[4], bool female) {
  void* system = natives.entity_system ? natives.entity_system() : nullptr;
  if (!system || !natives.spawn_params_init) {
    return nullptr;
  }
  void* registry = at_offset<Get>(system, kClassRegistry)(system);
  void* klass =
      registry ? at_offset<FindClass>(registry, kFindClass)(registry, entity_class) : nullptr;
  if (!klass) {
    return nullptr;
  }
  alignas(16) std::uint8_t params[kSpawnParamsSize];
  reinterpret_cast<SpawnParamsInit>(natives.spawn_params_init)(params);
  const char* name_text = name.c_str();
  std::memcpy(params + kSpawnClass, &klass, sizeof(klass));
  std::memcpy(params + kSpawnName, &name_text, sizeof(name_text));
  std::uint32_t flags = 0;
  std::memcpy(&flags, params + kSpawnFlags, sizeof(flags));
  flags |= kSpawnScriptFlags;
  std::memcpy(params + kSpawnFlags, &flags, sizeof(flags));
  std::memcpy(&flags, params + kSpawnFlagsExtended, sizeof(flags));
  flags |= kSpawnScriptFlagsExtended;
  std::memcpy(params + kSpawnFlagsExtended, &flags, sizeof(flags));
  std::memcpy(params + kSpawnPosition, position, 3 * sizeof(float));
  std::memcpy(params + kSpawnRotation, rotation, 4 * sizeof(float));
  OwnSpawn own;
  void* entity = at_offset<SpawnEntity>(system, kSpawnEntity)(system, params, false);
  if (!entity) {
    return nullptr;
  }
  if (!soul.empty()) {
    set_soul(entity, soul);
  }
  if (female) {
    make_female(entity);
  }
  if (!at_offset<InitEntity>(system, kInitEntity)(system, entity, params)) {
    return nullptr;
  }
  return entity;
}

bool set_script_flag(void* entity, const char* key, bool on) {
  void* script = entity ? script_table_of(entity) : nullptr;
  if (!script) {
    return false;
  }
  alignas(16) std::uint8_t value[kAnySize] = {};
  std::memcpy(value, &kAnyBoolean, sizeof(kAnyBoolean));
  std::memcpy(value + kAnyValue, &on, sizeof(on));
  slot<SetValueAny>(script, kSetValueAnySlot)(script, key, value, false);
  return true;
}

}  // namespace kcds::runtime::npcs
