// How loud the web views play (web_audio.hpp): the player's own Browser volume setting (0 to 1)
// times the server's share for this player (0 to 100), nothing while the server mutes them.
#pragma once

#include <algorithm>
#include <cmath>

namespace kcds::runtime::web::audio {

inline float clamped(float value, float least, float most) {
  if (!std::isfinite(value)) {
    return most;
  }
  return std::clamp(value, least, most);
}

inline float level(float setting, float server_volume, bool server_muted) {
  if (server_muted) {
    return 0.0f;
  }
  return clamped(setting, 0.0f, 1.0f) * clamped(server_volume, 0.0f, 100.0f) / 100.0f;
}

}  // namespace kcds::runtime::web::audio
