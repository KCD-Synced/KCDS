// The engine's deferred snow shader (Engine/Shaders.pak, DeferredSnow.cfx) covered a surface only
// where a 3D noise was high: its cover was
//   OUT.Blending.a = fIceTopZ * saturate(fFrostVariation * 8.h);
// and the noise (a product of three samples) is near 0 over large patches, so roofs and ground
// stayed bare in blotches even at the greatest amount. KCDS shows a winter, not a light frost:
// the cover follows the amount (10 covers every face that looks up) and the noise only varies it
// a little. The game compiles the shader on the player's PC (shader_compile.hpp); its HLSL text
// is changed here before it reaches the compiler. Header only, so the test reads it without the
// game.
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

namespace detail {

inline bool is_space(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

}  // namespace detail

// `source` with the snow's cover statement (the assignment to OUT.Blending.a that reads
// fIceTopZ) written as kCover; empty when the text is not the snow shader or has no such
// statement.
inline std::string with_full_cover(std::string_view source) {
  if (source.find("fIceTopZ") == std::string_view::npos) {
    return {};
  }
  constexpr std::string_view kTarget = "OUT.Blending.a";
  std::size_t at = source.find(kTarget);
  while (at != std::string_view::npos) {
    std::size_t next = at + kTarget.size();
    while (next < source.size() && detail::is_space(source[next])) {
      ++next;
    }
    // An assignment ("="), not a comparison ("==") nor another member.
    const bool assigned = next < source.size() && source[next] == '=' &&
                          (next + 1 >= source.size() || source[next + 1] != '=');
    const std::size_t end = assigned ? source.find(';', next) : std::string_view::npos;
    if (end != std::string_view::npos &&
        source.substr(next, end - next).find("fIceTopZ") != std::string_view::npos) {
      std::string out;
      out.reserve(source.size() + kCover.size());
      out.append(source.substr(0, at));
      out.append(kCover);
      out.append(source.substr(end + 1));
      return out;
    }
    at = source.find(kTarget, at + kTarget.size());
  }
  return {};
}

}  // namespace kcds::runtime::winter::snow_cover
