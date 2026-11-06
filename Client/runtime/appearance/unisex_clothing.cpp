#include "unisex_clothing.hpp"

#include "MinHook.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kcds::runtime::appearance::unisex_clothing {
namespace {

using WearCheck = bool(__fastcall*)(void* klass, void* soul);
using AddComponent = char(__fastcall*)(void* manager, void** component, void* layer);
using GenderCall = std::uint8_t(__fastcall*)(void* object);

constexpr std::size_t kClassClothing = 208;
constexpr std::size_t kClothingGender = 24;
constexpr std::size_t kClothingGenderValid = 25;
constexpr std::uint8_t kMale = 1;
constexpr std::uint8_t kFemale = 2;

// The clothing manager's owner (manager + 8) and the gender call of it and of a soul.
constexpr std::size_t kManagerOwner = 8;
constexpr std::size_t kOwnerGenderSlot = 7;
constexpr std::size_t kSoulGenderSlot = 41;

WearCheck original_wear_check = nullptr;
AddComponent original_add_component = nullptr;

template <class T>
T read(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

std::uint8_t gender_of(void* object, std::size_t slot) {
  void* const* vtable = read<void* const*>(object, 0);
  return reinterpret_cast<GenderCall>(vtable[slot])(object);
}

// Makes a male component say female until it is dropped, for a female wearer.
class FemaleForTheCall {
 public:
  FemaleForTheCall(std::uint8_t* component, std::uint8_t wearer) {
    if (component && wearer == kFemale && component[kClothingGenderValid] &&
        component[kClothingGender] == kMale) {
      component[kClothingGender] = kFemale;
      flipped_ = component;
    }
  }

  ~FemaleForTheCall() {
    if (flipped_) {
      flipped_[kClothingGender] = kMale;
    }
  }

  FemaleForTheCall(const FemaleForTheCall&) = delete;
  FemaleForTheCall& operator=(const FemaleForTheCall&) = delete;

 private:
  std::uint8_t* flipped_ = nullptr;
};

bool __fastcall female_wear_check(void* klass, void* soul) {
  auto* component = klass ? read<std::uint8_t*>(klass, kClassClothing) : nullptr;
  const std::uint8_t wearer = soul ? gender_of(soul, kSoulGenderSlot) : kMale;
  FemaleForTheCall female(component, wearer);
  return original_wear_check(klass, soul);
}

char __fastcall female_add_component(void* manager, void** component, void* layer) {
  void* owner = manager ? read<void*>(manager, kManagerOwner) : nullptr;
  const std::uint8_t wearer = owner ? gender_of(owner, kOwnerGenderSlot) : kMale;
  FemaleForTheCall female(component ? static_cast<std::uint8_t*>(*component) : nullptr, wearer);
  return original_add_component(manager, component, layer);
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install(void* wear_check, void* add_component) {
  const bool wear = hook(wear_check, reinterpret_cast<void*>(female_wear_check),
                         reinterpret_cast<void**>(&original_wear_check));
  const bool add = hook(add_component, reinterpret_cast<void*>(female_add_component),
                        reinterpret_cast<void**>(&original_add_component));
  return wear && add;
}

}  // namespace kcds::runtime::appearance::unisex_clothing
