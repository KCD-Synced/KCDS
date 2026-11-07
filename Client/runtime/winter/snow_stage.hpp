// Snow on the game's own world: the renderer's deferred snow stage (CryEngine's CSnowStage,
// shader DeferredSnow), which the game ships but never turns on. Each frame the stage copies the
// snow parameters the 3D engine gave the renderer (all zero in the game) into itself; KCDS lets
// that copy happen and then puts its own over it, so the stage draws snow on every surface that
// faces up (ground, roofs, branches) and snow falling around the camera.
//
// The 56 bytes the stage keeps at + 0x3EC0 (SSnowInfo): the snow volume's centre (3 floats) and
// radius, the snow amount on surfaces, frost, surface freezing, the falling flakes' count (int),
// size, brightness, gravity, wind and turbulence strength and frequency.
//
// set any thread; the hook runs on the render thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::winter::snow {

using Logger = void (*)(const char* format, ...);

struct Settings {
  // On surfaces: 0 none, 1 a light cover, up to 10 deep snow.
  float amount = 0.0f;
  // Frost on surfaces and how frozen they look, 0 to 1.
  float frost = 0.0f;
  float freezing = 0.0f;
  // Falling snow: flakes (0 none), their size, brightness and how they fall.
  int flakes = 0;
  float flake_size = 1.0f;
  float brightness = 1.0f;
  float gravity = 0.1f;
  float wind = 0.1f;
  float turbulence = 0.5f;
  float turbulence_frequency = 0.1f;
};

// Hooks the stage's update, its is-active check and the settings' "r_snow above 0" question
// (resolved by signature: the stage's settings keep r_snow 0, so the falling flakes' pass never
// ran); false when one is not there. console_slot: where the game keeps its console
// (gEnv->pConsole). stencil_operand: the stencil test the snow's pass asks for in the game's code
// (equal to the scene's mark, which trees, bushes and brushwood never carry), made "always" so
// snow lies on them too; null leaves it.
bool install(void* update, void* is_active, void* enabled, void* stencil_operand,
             void** console_slot, Logger logger);

// The snow from now on (nothing when amount, frost, freezing and flakes are all 0). The game ships
// the snow shaders uncompiled: the first time snow is wanted, its own shader compiler is allowed
// (r_ShadersAllowCompilation 1, winter/shader_compile.hpp) and its warnings are no errors
// (r_shadersWarningsAsErrorsWH 0). Engine thread.
void set(const Settings& settings);

// The console settings the snow needs (shader compiling, r_Snow, the grass buried under deep
// snow), set again until the game's console takes them: snow that comes with the join arrives
// before the game made its console. Engine thread, every frame.
void settle();

// Where the snow is centred (the camera's place; engine thread, every frame).
void follow(float x, float y, float z);

// Whether snow is shown now.
bool active();

}  // namespace kcds::runtime::winter::snow
