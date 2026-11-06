#include "station_prompts.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../input/interaction_rules.hpp"
#include "MinHook.h"
#include "kcd2s/integration/interaction_rules.hpp"

namespace kcds::runtime::interactions::stations {
namespace {

namespace rules = kcds::runtime::input::interaction_rules;
namespace wire = kcd2s::integration;

// The interactor's focus entity (its update's + 244).
constexpr std::size_t kInteractorFocus = 244;

// The food stations' triggers say what they are in their script's Misc text
// (Scripts/Entities/WH/Triggers/FoodProcessingTrigger.lua).
constexpr char kCooking[] = "foodProcessingType:cooking";
constexpr char kSmoking[] = "foodProcessingType:smoking";
constexpr char kDrying[] = "foodProcessingType:drying";

// The reader of an entity's Properties takes an object with the entity at + 8.
struct EntityHolder {
  void* unused = nullptr;
  void* entity = nullptr;
};
using InteractorStep = std::int64_t(__fastcall*)(void* interactor, std::uint32_t* entity);
using EntityProperties = void**(__fastcall*)(EntityHolder* holder, void** table);
using SubTable = bool(__fastcall*)(void* table, const char* name, void** out);
// IScriptTable's GetValueAny (+ 64; the game's own reader): a value and its type, text
// is 5.
struct ScriptValue {
  std::int32_t type = 5;
  const char* text = nullptr;
  std::uint8_t rest[16] = {};
};
using GetValue = bool(__fastcall*)(void* table, const char* name, ScriptValue* value, bool chain);
constexpr std::size_t kTableGetValue = 64;
// IScriptTable's Release: every table reference these natives hand out holds one.
constexpr std::size_t kTableRelease = 32;

void release(void* table) {
  if (table) {
    using Release = void(__fastcall*)(void*);
    (*reinterpret_cast<Release*>(*static_cast<std::uint8_t**>(table) + kTableRelease))(table);
  }
}

Natives natives;
InteractorStep original_step = nullptr;
// The last entity asked about and the rule its type answers to (0 for none).
std::uint32_t known_entity = 0;
std::uint16_t known_rule = 0;

std::uint16_t rule_of(std::uint32_t id) {
  if (id == known_entity) {
    return known_rule;
  }
  known_entity = id;
  known_rule = 0;
  void* entity = natives.entity(id);
  if (!entity) {
    return 0;
  }
  EntityHolder holder;
  holder.entity = entity;
  void* properties = nullptr;
  void* script = nullptr;
  ScriptValue value;
  reinterpret_cast<EntityProperties>(natives.properties)(&holder, &properties);
  if (properties && reinterpret_cast<SubTable>(natives.sub_table)(properties, "Script", &script) &&
      script) {
    (*reinterpret_cast<GetValue*>(*static_cast<std::uint8_t**>(script) + kTableGetValue))(
        script, "Misc", &value, false);
  }
  const char* misc = value.type == 5 ? value.text : nullptr;
  if (misc && std::strstr(misc, kCooking)) {
    known_rule = wire::kRefuseCooking;
  } else if (misc && (std::strstr(misc, kSmoking) || std::strstr(misc, kDrying))) {
    known_rule = wire::kRefuseSmoking;
  }
  release(script);
  release(properties);
  return known_rule;
}

std::int64_t __fastcall hooked_step(void* interactor, std::uint32_t* entity) {
  const std::uint32_t id = entity ? *entity : 0;
  const std::uint16_t rule = id ? rule_of(id) : 0;
  if (rule == 0 || !rules::allowed(rule)) {
    return original_step(interactor, entity);
  }
  // Not the focus: the reference goes back as it was, for the update's own clean-up.
  *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(interactor) + kInteractorFocus) = 0;
  *entity = 0;
  const std::int64_t result = original_step(interactor, entity);
  *entity = id;
  return result;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  if (!natives.interactor_step || !natives.properties || !natives.sub_table || !natives.entity) {
    if (logger) {
      logger("INTERACTIONS station prompts unavailable");
    }
    return false;
  }
  const bool hooked = MH_CreateHook(natives.interactor_step, reinterpret_cast<void*>(&hooked_step),
                                    reinterpret_cast<void**>(&original_step)) == MH_OK &&
                      MH_EnableHook(natives.interactor_step) == MH_OK;
  if (logger) {
    logger("INTERACTIONS station prompts %s", hooked ? "hooked" : "unavailable");
  }
  return hooked;
}

}  // namespace kcds::runtime::interactions::stations
