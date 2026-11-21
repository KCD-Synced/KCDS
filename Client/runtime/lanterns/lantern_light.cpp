#include "lantern_light.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#include "../props/prop_entity.hpp"
#include "../vfx/placed_effects.hpp"

namespace kcds::runtime::lanterns::light {
namespace {

namespace entity = kcds::runtime::props::entity;

// SChildAttachParams with no flags: on the parent entity itself.
struct AttachParams {
  std::int32_t flags;
  const char* bone;
};
using AttachChild = bool(__fastcall*)(void* parent, void* child, const AttachParams* params);

std::string light_name(std::uint32_t id) {
  return "kcds_prop_light_" + std::to_string(id);
}

// Standard.Light_Lantern's colour.
constexpr float kColour[3] = {0.887923f, 0.337164f, 0.124772f};
// The game's lamp (Scripts/Entities/WH/Torch/Lamp.lua): the light and a candle flame at the
// model's helper slt_particle. Its glass and flame look lit by their own material, so the light
// itself is weak.
constexpr char kFlameHelper[] = "slt_particle";
constexpr char kCandle[] = "WH_Particels.fires.candle";

}  // namespace

bool is_lantern(const std::string& model) {
  std::string lower = model;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  const std::size_t slash = lower.find_last_of('/');
  const std::string file = slash == std::string::npos ? lower : lower.substr(slash + 1);
  return file.rfind("lamp_", 0) == 0 && file.find("_wearable") != std::string::npos;
}

bool light(std::uint32_t id, void* lamp, void* attach_child) {
  put_out(id);
  if (!lamp || !attach_child) {
    return false;
  }
  float at[3];
  entity::world_position(lamp, at);
  const float turn[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  // The archetype's values, by the Light script's Properties and its tables.
  const std::vector<entity::ScriptProperty> properties = {
      {"bActive", nullptr, 1.0f, nullptr, true},
      {"Radius", nullptr, 6.0f},
      {"fAttenuationBulbSize", nullptr, 0.35f},
      {"clrDiffuse", nullptr, 0.0f, "Color", false, kColour},
      {"fDiffuseMultiplier", nullptr, 0.003f, "Color"},
      {"fSpecularMultiplier", nullptr, 1.5f, "Color"},
      {"fVolumetricMultiplier", nullptr, 4.0f, "Color"},
      {"bIgnoresVisAreas", nullptr, 1.0f, "Options", true},
      {"bVolumetricFog", nullptr, 1.0f, "Options", true},
      {"nCastShadows", nullptr, 0.0f, "Shadows"},
      {"nLightStyle", nullptr, 49.0f, "Style"},
      {"fAnimationSpeed", nullptr, 0.35f, "Style"},
      {"nAnimationPhase", nullptr, 87.0f, "Style"},
  };
  void* made = entity::spawn_class_with(light_name(id), "Light", at, turn, properties);
  if (!made) {
    return false;
  }
  const AttachParams params{0, nullptr};
  reinterpret_cast<AttachChild>(attach_child)(lamp, made, &params);
  // Where the game's lamp lights it: the lamp model's flame helper, its light and its candle.
  float flame[3] = {0.0f, 0.0f, 0.0f};
  float flame_turn[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  if (!entity::helper_pose(lamp, kFlameHelper, flame, flame_turn)) {
    std::memcpy(flame_turn, turn, sizeof(flame_turn));
  }
  entity::place_rotated(made, flame, flame_turn);
  vfx::placed::load_effect(made, kCandle);
  return true;
}

void put_out(std::uint32_t id) {
  entity::remove(light_name(id));
}

}  // namespace kcds::runtime::lanterns::light
