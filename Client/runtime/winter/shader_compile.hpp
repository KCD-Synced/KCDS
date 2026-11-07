// The game's own shader compiles (DXC, Bin/Win64Shared/dxcompiler.dll), for the shaders its cache
// ships without: the deferred snow stage's (winter/snow_stage.hpp). The game compiles a missing
// shader itself when r_ShadersAllowCompilation is on, but its DXC (1.7.2308) reads HLSL 2021 by
// default, and the engine's snow shader is written for the older rules (a vector condition in
// ?:), so DeferredSnowPS failed and the game kept an empty cache entry. Every compile now gets
// "-HV 2018" after the game's own arguments; the shaders the game ships compiled are never
// compiled again, so nothing else changes. The snow's caller also turns the game's
// r_shadersWarningsAsErrorsWH off: the shader's implicit vector truncations are warnings.
// The compiled shaders go to the player's own shader cache (Saved Games\kingdomcome2\shaders),
// once.
#pragma once

namespace kcds::runtime::winter::shader_compile {

using Logger = void (*)(const char* format, ...);

// Hooks the game's DXC compile (`compile`, resolved by signature); false when it is not there.
bool install(void* compile, Logger logger);

}  // namespace kcds::runtime::winter::shader_compile
