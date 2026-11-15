#include "fog.hpp"

#include <atomic>
#include <cmath>

#include "../core/log.hpp"
// Last: it brings Windows.h, whose min and max macros the headers above must not see.
#include "MinHook.h"

namespace kcds::runtime::world::fog {
namespace {

// The engine's SetGlobalParameter in its vtable.
constexpr int kSetGlobalParameterSlot = 224;

struct Vec3 {
  float x;
  float y;
  float z;
};

// The parameters of a foggy day the time of day hands the engine, and what the game's own fog
// preset (foggy_storm_no_rain, measured at noon against cloudless_sunny) gives each. Fog alone
// under a clear sky took the sky's blue: the game's fog lies under a closed cloud cover, in dim
// light. `foggy` is the preset's value, or for the light (`scaled`, which follows the hour) what
// the preset multiplies it by.
//   7 fog colour; 9, 10 the height fog's bottom and top layers (height, density); 11 its radial
//   lobe; 12 the volumetric fog's global density (x); 13 its ramp (the distances from the camera
//   where it starts and is full); 18 the second volumetric fog's factors; 19 its ramp; 21 its
//   global density; 23 its top layer; 25 its sun radial colour; 26 the sky's light; 40 the sun's;
//   44 to 50 and 55 the clouds (cover, altitude, thickness, their light and shape); 72, 93, 97,
//   98 the light's multipliers; 76, 77, 79, 80 the film curve and exposure, 82 bloom and 85, 99
//   the white photo filter, which make the fog bright. The preset's colour balance (84, blue)
//   stays the weather's: with it the fog turned lilac.
struct FogParameter {
  int id;
  Vec3 foggy;
  bool scaled;
};
constexpr FogParameter kParameters[] = {
    {7, {4.7365f, 0.0f, 4.7365f}, false},     {9, {-100.0f, 0.5f, 0.0f}, false},
    {10, {1000.0f, 0.05f, 0.0f}, false},      {11, {0.0f, 0.95f, 1.0f}, false},
    {12, {0.3f, 0.0f, 1.0f}, false},          {13, {30.0f, 40.0f, 1.0f}, false},
    {18, {1.0f, 0.0789f, 0.7f}, false},       {19, {0.0f, 0.0f, 0.0f}, false},
    {21, {4.5f, 1.0f, 0.5f}, false},          {23, {5500.0f, 0.0f, 0.7072f}, false},
    {25, {0.2735f, 0.2194f, 0.1661f}, false}, {26, {0.0416f, 0.0407f, 0.0401f}, true},
    {40, {0.2403f, 0.2782f, 0.2784f}, true},  {44, {0.75f, 500.0f, 4000.0f}, false},
    {45, {1.0f, 3.0f, 0.8f}, false},          {46, {1.0f, 0.0f, 0.5f}, false},
    {47, {0.1147f, 0.1737f, 0.3515f}, false}, {48, {1.0f, 0.2f, 10.0f}, false},
    {49, {55.0f, 0.0f, 71.2977f}, false},     {50, {0.9f, 0.1f, 0.01f}, false},
    {55, {0.5f, 0.0f, 0.0f}, false},          {72, {0.0f, 0.0f, 0.0f}, true},
    {76, {0.7f, 0.0f, 0.0f}, false},          {77, {1.3f, 0.0f, 0.0f}, false},
    {79, {8.0f, 0.0f, 0.0f}, false},          {80, {6.5f, 7.5f, 3.0f}, false},
    {82, {6.0f, 0.0f, 0.0f}, false},          {85, {1.0f, 1.0f, 1.0f}, false},
    {93, {1.25f, 1.25f, 1.25f}, true},        {97, {0.125f, 0.125f, 0.125f}, true},
    {98, {0.1877f, 0.1358f, 0.0706f}, true},  {99, {1.0f, 0.0f, 0.0f}, false},
};
constexpr int kParameterCount = int(sizeof(kParameters) / sizeof(kParameters[0]));
constexpr int kFogDensity = 12;
constexpr int kFogRamp = 13;
constexpr int kFog2Factors = 18;
constexpr int kFog2Density = 21;
// Up to strength 0.5 the fog goes from the weather's to the game's own fog; above it the fog's
// densities grow up to this many times that, and its ramp comes in to the eye.
constexpr float kFoggyAt = 0.5f;
constexpr float kMostThicker = 8.0f;
constexpr Vec3 kNearRamp{0.0f, 4.0f, 1.0f};

float lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
  return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
}

using SetGlobalParameter = void(__fastcall*)(void* engine, int parameter, const Vec3* value);
SetGlobalParameter original_set = nullptr;
bool hooked = false;
std::atomic<float> wanted{0.0f};
// The strength the engine last got (frame), and what the time of day last gave for each fog
// parameter: it gives them only when it updates (not while the clock stands still), so a new
// strength is put in with its last values.
float pushed = 0.0f;
Vec3 last_values[kParameterCount]{};
bool have_values[kParameterCount]{};

int index_of(int parameter) {
  for (int i = 0; i < kParameterCount; ++i) {
    if (kParameters[i].id == parameter) {
      return i;
    }
  }
  return -1;
}

// The time of day's value made into the fog of `strength` (above 0).
Vec3 thickened(int index, const Vec3& value, float strength) {
  const FogParameter& parameter = kParameters[index];
  Vec3 foggy = parameter.foggy;
  if (parameter.scaled) {
    foggy = {value.x * foggy.x, value.y * foggy.y, value.z * foggy.z};
  }
  Vec3 fog = lerp(value, foggy, std::fmin(1.0f, strength / kFoggyAt));
  if (strength <= kFoggyAt) {
    return fog;
  }
  const float beyond = (strength - kFoggyAt) / (1.0f - kFoggyAt);
  const float thicker = lerp(1.0f, kMostThicker, beyond);
  if (parameter.id == kFogDensity || parameter.id == kFog2Density) {
    fog.x *= thicker;
  } else if (parameter.id == kFog2Factors) {
    fog.y *= thicker;
  } else if (parameter.id == kFogRamp) {
    fog = lerp(fog, kNearRamp, beyond);
  }
  return fog;
}

void __fastcall hooked_set(void* engine, int parameter, const Vec3* value) {
  const int index = value ? index_of(parameter) : -1;
  if (index >= 0) {
    last_values[index] = *value;
    have_values[index] = true;
    const float strength = wanted.load();
    if (strength > 0.0f) {
      const Vec3 thicker = thickened(index, *value, strength);
      original_set(engine, parameter, &thicker);
      return;
    }
  }
  original_set(engine, parameter, value);
}

}  // namespace

void want(float strength) {
  const float clamped =
      std::isfinite(strength) && strength > 0.0f ? (strength > 1.0f ? 1.0f : strength) : 0.0f;
  if (wanted.exchange(clamped) != clamped) {
    kcds::game_client::log("WORLD fog %.2f", double(clamped));
  }
}

void frame(void* engine) {
  if (!engine) {
    return;
  }
  if (hooked) {
    // A new strength reaches the engine at once, with the time of day's last fog under it.
    const float strength = wanted.load();
    if (original_set && strength != pushed) {
      pushed = strength;
      for (int i = 0; i < kParameterCount; ++i) {
        if (have_values[i]) {
          const Vec3 value = last_values[i];
          hooked_set(engine, kParameters[i].id, &value);
        }
      }
    }
    return;
  }
  hooked = true;
  void* target = (*static_cast<void***>(engine))[kSetGlobalParameterSlot];
  const bool done = target &&
                    MH_CreateHook(target, reinterpret_cast<void*>(&hooked_set),
                                  reinterpret_cast<void**>(&original_set)) == MH_OK &&
                    MH_EnableHook(target) == MH_OK;
  kcds::game_client::log("WORLD fog %s", done ? "installed" : "unavailable");
}

}  // namespace kcds::runtime::world::fog
