// The engine's deferred snow shader (Engine/Shaders.pak, DeferredSnow.cfx) as KCDS draws it. The
// game compiles the shader on the player's PC (shader_compile.hpp); its HLSL text is changed here
// before it reaches the compiler. Two of its statements are written otherwise:
//
// - Its cover, OUT.Blending.a = fIceTopZ * saturate(fFrostVariation * 8.h): a surface was covered
//   only where a 3D noise was high, and the noise (a product of three samples) is near 0 over
//   large patches, so roofs and ground stayed bare in blotches even at the greatest amount. KCDS
//   shows a winter, not a light frost: the cover follows the amount (10 covers every face that
//   looks up) and the noise only varies it a little.
// - Its volume, fAttenuation = saturate(GetAttenuation(g_SnowVolumeParams.xyz - vPosition.xyz,
//   g_SnowVolumeParams.w)): snow inside a sphere around the camera. KCDS keeps that sphere
//   larger than the world (the pass draws only where it lies on the screen) and uses its centre
//   for a box without snow instead, the roof the camera stands under (snow_shelter.hpp): x and y
//   the box's middle, z its size and turn written as one number (box_code), 0 in the open; snow
//   everywhere else. The code stays below 512 so the sphere, 4 km wide, still holds the world
//   around the camera.
//
// Header only, so the test reads it without the game.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace kcds::runtime::winter::snow_cover {

// The box's code: its half length, half width (quarter metres, 0 to 127 each) and turn (128ths of
// a half circle) as the bits of a whole number, divided by 4096. Every step is a power of two, so
// the shader takes it apart exactly.
constexpr float kBoxUnit = 0.25f;
constexpr float kBoxScale = 4096.0f;
constexpr std::uint32_t kBoxSteps = 128;

// The code of a box: half length and half width in metres (rounded up to a step, at most 31.75 m),
// turn in radians, 0 to pi. Never 0, which means no box.
inline float box_code(float half_length, float half_width, float turn) {
  const auto steps = [](float metres) {
    const float count = std::ceil(metres / kBoxUnit);
    return static_cast<std::uint32_t>((std::clamp)(count, 1.0f, float(kBoxSteps - 1)));
  };
  const float half_circle = 3.14159265f;
  const float turned = std::round(turn / half_circle * float(kBoxSteps));
  const auto turn_step = static_cast<std::uint32_t>(turned) % kBoxSteps;
  const std::uint32_t code =
      (steps(half_length) * kBoxSteps + steps(half_width)) * kBoxSteps + turn_step;
  return float(code) / kBoxScale;
}

// The box a code gives, as the shader reads it: half length, half width (metres), turn (radians).
struct Box {
  float half_length;
  float half_width;
  float turn;
};

inline Box box_of(float code) {
  const float whole = std::round(code * kBoxScale);
  const float length_steps = std::floor(whole / float(kBoxSteps * kBoxSteps));
  const float rest = whole - length_steps * float(kBoxSteps * kBoxSteps);
  const float width_steps = std::floor(rest / float(kBoxSteps));
  const float turn_steps = rest - width_steps * float(kBoxSteps);
  return {length_steps * kBoxUnit, width_steps * kBoxUnit,
          turn_steps * 3.14159265f / float(kBoxSteps)};
}

// The snow shader's cover, as KCDS draws it: the amount (fAmount, 0 to 10) decides, the frost
// noise takes off at most a tenth.
constexpr std::string_view kPlainCover =
    "OUT.Blending.a = fIceTopZ * saturate(fAmount * 0.1h * (0.9h + 0.1h * "
    "saturate(fFrostVariation * 8.h)));";
// The same with far foliage covered too. A face is covered as far as it looks up (fIceTopZ), but
// the far forest is drawn as simpler trees whose faces mostly look at the camera, so it stayed
// green under a white land: foliage (the G-buffer's transmittance lighting model, 1) is covered
// more with its distance, from 40 m on to nine tenths at 200 m and beyond. Ground, walls and rock
// keep their own faces.
constexpr std::string_view kCover =
    "{\n"
    "  min16float kcdsTop = fIceTopZ;\n"
    "  if (attribs.LightingModel == 1) {\n"
    // The camera's place as the game's own text reads it (its vView).
    "    float3 kcdsCamera = float3(CV_ScreenToWorldBasis[0][3], CV_ScreenToWorldBasis[1][3], "
    "CV_ScreenToWorldBasis[2][3]);\n"
    "    float kcdsFar = saturate((length(vPosition.xyz - kcdsCamera) - 40.0f) / 160.0f);\n"
    "    kcdsTop = max(kcdsTop, (min16float)(0.9f * kcdsFar));\n"
    "  }\n"
    "  OUT.Blending.a = kcdsTop * saturate(fAmount * 0.1h * (0.9h + 0.1h * "
    "saturate(fFrostVariation * 8.h)));\n"
    "}";
// The snow's volume, as KCDS uses it: none inside the box (box_of in HLSL), and no straight edge
// where the roof ends: snow blown under eaves lies ragged and thins out. A value noise of the
// ground's own place (three octaves, 1.25 m cells and finer) moves the edge by up to about a
// metre either way, and the snow thins over 1.2 m (in full floats: the distances are hundreds of
// metres).
constexpr std::string_view kVolume =
    "fAttenuation = 1.0h;\n"
    "{\n"
    "  float kcdsCode = round(g_SnowVolumeParams.z * 4096.0f);\n"
    "  if (kcdsCode > 0.5f) {\n"
    "    float kcdsLength = floor(kcdsCode / 16384.0f);\n"
    "    float kcdsRest = kcdsCode - kcdsLength * 16384.0f;\n"
    "    float kcdsWidth = floor(kcdsRest / 128.0f);\n"
    "    float kcdsTurn = (kcdsRest - kcdsWidth * 128.0f) * 3.14159265f / 128.0f;\n"
    "    float2 kcdsAlong = float2(cos(kcdsTurn), sin(kcdsTurn));\n"
    "    float2 kcdsAway = vPosition.xy - g_SnowVolumeParams.xy;\n"
    "    float2 kcdsOn = abs(float2(dot(kcdsAway, kcdsAlong), "
    "dot(kcdsAway, float2(-kcdsAlong.y, kcdsAlong.x))));\n"
    "    float2 kcdsOut = kcdsOn - float2(kcdsLength, kcdsWidth) * 0.25f;\n"
    "    float kcdsNoise = 0.0f;\n"
    "    float kcdsScale = 0.8f;\n"
    "    float kcdsWeight = 0.65f;\n"
    "    [unroll] for (int kcdsOctave = 0; kcdsOctave < 3; ++kcdsOctave) {\n"
    "      float2 kcdsCell = vPosition.xy * kcdsScale;\n"
    "      float2 kcdsBase = floor(kcdsCell);\n"
    "      float2 kcdsPart = kcdsCell - kcdsBase;\n"
    "      kcdsPart = kcdsPart * kcdsPart * (3.0f - 2.0f * kcdsPart);\n"
    "      float4 kcdsCorner = 0.0f;\n"
    "      [unroll] for (int kcdsK = 0; kcdsK < 4; ++kcdsK) {\n"
    "        float2 kcdsAt = kcdsBase + float2((float)(kcdsK & 1), (float)(kcdsK >> 1));\n"
    "        float3 kcdsHash = frac(kcdsAt.xyx * 0.1031f);\n"
    "        kcdsHash += dot(kcdsHash, kcdsHash.yzx + 33.33f);\n"
    "        kcdsCorner[kcdsK] = frac((kcdsHash.x + kcdsHash.y) * kcdsHash.z);\n"
    "      }\n"
    "      float kcdsValue = lerp(lerp(kcdsCorner.x, kcdsCorner.y, kcdsPart.x), "
    "lerp(kcdsCorner.z, kcdsCorner.w, kcdsPart.x), kcdsPart.y);\n"
    "      kcdsNoise += (kcdsValue - 0.5f) * kcdsWeight;\n"
    "      kcdsScale *= 2.7f;\n"
    "      kcdsWeight *= 0.45f;\n"
    "    }\n"
    "    float kcdsEdge = max(kcdsOut.x, kcdsOut.y) + kcdsNoise * 2.0f;\n"
    "    fAttenuation = (min16float)saturate((kcdsEdge + 0.3f) / 1.2f);\n"
    "  }\n"
    "}";

namespace detail {

inline bool is_space(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

// `source` with the first assignment to `target` whose value reads `reads` written as
// `replacement` (from `target` to its ";"), into `out`; false when there is none.
inline bool replace_assignment(std::string_view source, std::string_view target,
                               std::string_view reads, std::string_view replacement,
                               std::string& out) {
  std::size_t at = source.find(target);
  while (at != std::string_view::npos) {
    std::size_t next = at + target.size();
    while (next < source.size() && is_space(source[next])) {
      ++next;
    }
    // An assignment ("="), not a comparison ("==") nor another member.
    const bool assigned = next < source.size() && source[next] == '=' &&
                          (next + 1 >= source.size() || source[next + 1] != '=');
    const std::size_t end = assigned ? source.find(';', next) : std::string_view::npos;
    if (end != std::string_view::npos &&
        source.substr(next, end - next).find(reads) != std::string_view::npos) {
      std::string written;
      written.reserve(source.size() + replacement.size());
      written.append(source.substr(0, at));
      written.append(replacement);
      written.append(source.substr(end + 1));
      out = std::move(written);
      return true;
    }
    at = source.find(target, at + target.size());
  }
  return false;
}

}  // namespace detail

// `source` with the snow's cover (kCover, or kPlainCover without `far_foliage`) and volume
// (kVolume) written as KCDS's; empty when the text is not the snow shader or lacks either
// statement.
inline std::string with_full_cover(std::string_view source, bool far_foliage = true) {
  if (source.find("fIceTopZ") == std::string_view::npos) {
    return {};
  }
  std::string covered;
  const std::string_view cover = far_foliage ? kCover : kPlainCover;
  if (!detail::replace_assignment(source, "OUT.Blending.a", "fIceTopZ", cover, covered)) {
    return {};
  }
  std::string out;
  if (!detail::replace_assignment(covered, "fAttenuation", "GetAttenuation", kVolume, out)) {
    return {};
  }
  return out;
}

}  // namespace kcds::runtime::winter::snow_cover
