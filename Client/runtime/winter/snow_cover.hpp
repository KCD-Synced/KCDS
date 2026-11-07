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
//   for a column without snow instead: x and y its middle, z its radius (0 in the open), the
//   roof the camera stands under (snow_shelter.hpp); snow everywhere else.
//
// Header only, so the test reads it without the game.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace kcds::runtime::winter::snow_cover {

// The snow shader's cover, as KCDS draws it: the amount (fAmount, 0 to 10) decides, the frost
// noise takes off at most a tenth.
constexpr std::string_view kCover =
    "OUT.Blending.a = fIceTopZ * saturate(fAmount * 0.1h * (0.9h + 0.1h * "
    "saturate(fFrostVariation * 8.h)));";
// The snow's volume, as KCDS uses it: none inside the column, a soft edge an eighth of its radius
// (in full floats: the distances are hundreds of metres).
constexpr std::string_view kVolume =
    "fAttenuation = saturate((length(g_SnowVolumeParams.xy - vPosition.xy) - "
    "g_SnowVolumeParams.z) * 8.0f / max(g_SnowVolumeParams.z, 0.1f));";

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

// `source` with the snow's cover (kCover) and volume (kVolume) written as KCDS's; empty when the
// text is not the snow shader or lacks either statement.
inline std::string with_full_cover(std::string_view source) {
  if (source.find("fIceTopZ") == std::string_view::npos) {
    return {};
  }
  std::string covered;
  if (!detail::replace_assignment(source, "OUT.Blending.a", "fIceTopZ", kCover, covered)) {
    return {};
  }
  std::string out;
  if (!detail::replace_assignment(covered, "fAttenuation", "GetAttenuation", kVolume, out)) {
    return {};
  }
  return out;
}

}  // namespace kcds::runtime::winter::snow_cover
