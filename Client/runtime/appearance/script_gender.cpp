#include "script_gender.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "MinHook.h"

namespace kcds::runtime::appearance::script_gender {
namespace {

// soul + 3256 its archetype (records 88 bytes apart in id order, the id at + 8, the gender at
// + 28: 1 male, 2 female; appearance/components.cpp).
constexpr std::size_t kSoulArchetype = 3256;
constexpr std::size_t kArchetypeSize = 88;
constexpr std::size_t kArchetypeId = 8;
constexpr std::size_t kArchetypeGender = 28;
constexpr std::uint8_t kGenderMale = 1;
constexpr std::uint8_t kGenderFemale = 2;
// The player's archetypes: Hero and Hero_female.
constexpr int kHero = 13;
constexpr int kHeroFemale = 15;

using GetGender = std::int64_t(__fastcall*)(void* self, void* handler);
using HandlerSoul = void*(__fastcall*)(void* handler);

Logger log_line = nullptr;
Natives natives;
GetGender original_get_gender = nullptr;

int id_of(const std::uint8_t* archetype) {
  int id = 0;
  std::memcpy(&id, archetype + kArchetypeId, sizeof(id));
  return id;
}

// The Hero record for a Hero_female one, or null.
std::uint8_t* hero_for(std::uint8_t* archetype) {
  if (!archetype || id_of(archetype) != kHeroFemale ||
      archetype[kArchetypeGender] != kGenderFemale) {
    return nullptr;
  }
  std::uint8_t* hero = archetype + (kHero - kHeroFemale) * std::ptrdiff_t(kArchetypeSize);
  return id_of(hero) == kHero && hero[kArchetypeGender] == kGenderMale ? hero : nullptr;
}

std::int64_t __fastcall hooked_get_gender(void* self, void* handler) {
  void* soul = handler ? reinterpret_cast<HandlerSoul>(natives.handler_soul)(handler) : nullptr;
  auto* bytes = static_cast<std::uint8_t*>(soul);
  std::uint8_t* archetype = nullptr;
  if (bytes) {
    std::memcpy(&archetype, bytes + kSoulArchetype, sizeof(archetype));
  }
  std::uint8_t* hero = hero_for(archetype);
  if (!hero) {
    return original_get_gender(self, handler);
  }
  std::memcpy(bytes + kSoulArchetype, &hero, sizeof(hero));
  const std::int64_t result = original_get_gender(self, handler);
  std::memcpy(bytes + kSoulArchetype, &archetype, sizeof(archetype));
  static bool noted = false;
  if (!noted && log_line) {
    noted = true;
    log_line("APPEARANCE scripts see the player's woman as a player (soul:GetGender male)");
  }
  return result;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.get_gender || !natives.handler_soul) {
    return false;
  }
  return MH_CreateHook(natives.get_gender, reinterpret_cast<void*>(&hooked_get_gender),
                       reinterpret_cast<void**>(&original_get_gender)) == MH_OK &&
         MH_EnableHook(natives.get_gender) == MH_OK;
}

}  // namespace kcds::runtime::appearance::script_gender
