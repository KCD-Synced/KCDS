#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "kcd2s/core/bits.hpp"

// A scripted camera shot for one player: the server places that
// player's camera at a point, aimed by a rotation or at a point to look at, with a field of view,
// and moves it there over some seconds; or gives the player's own view back. All three messages
// are on the control channel (0).
//
//   CAMERA_SHOT       : server -> a ready player's client: [tag][position 3 x f32][aim u8], then
//                       for aim 0 a rotation quaternion (4 x f32, x y z w; its roll included) or
//                       for aim 1 the point to look at (3 x f32) and the roll about the line of
//                       sight (f32, degrees, positive turns the view's right side down), then
//                       [fov f32][seconds f32][ease u8]. fov is in degrees, 0 keeps the game's
//                       own; seconds is how long the camera takes from where it is to the shot
//                       (0 cuts); ease 1 starts and ends the move slowly, 0 moves at one speed
//                       (chained shots then run on without stopping). A message
//                       without the ease byte is eased.
//   CAMERA_CLEAR      : server -> a ready player's client: [tag]. The player's own view comes back
//                       at once.
//   CAMERA_SHOT_ENDED : client -> server: [tag][reason u8]. A shot ended without the server
//                       clearing it: 0 = the world changed (loading screen), 1 = the game had no
//                       view to make it with, 2 = the shot's view was taken away.
//
// Client/runtime/camera/camera_shot.* implements the client side.
namespace kcd2s::integration {

inline constexpr std::uint8_t kCameraShotTag = 87;
inline constexpr std::uint8_t kCameraClearTag = 88;
inline constexpr std::uint8_t kCameraShotEndedTag = 89;

inline constexpr float kMaxCameraCoordinate = 100000.0f;
inline constexpr float kMinCameraFov = 5.0f;
inline constexpr float kMaxCameraFov = 150.0f;
inline constexpr float kMaxCameraSeconds = 60.0f;
inline constexpr float kMaxCameraRoll = 180.0f;

enum class CameraAim : std::uint8_t { Rotation = 0, LookAt = 1 };

enum class CameraShotEnd : std::uint8_t { WorldChanged = 0, Unavailable = 1, Lost = 2 };
inline constexpr std::uint8_t kCameraShotEndCount = 3;

struct CameraShot {
  float position[3] = {0.0f, 0.0f, 0.0f};
  CameraAim aim = CameraAim::Rotation;
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // x y z w, for CameraAim::Rotation
  float target[3] = {0.0f, 0.0f, 0.0f};          // for CameraAim::LookAt
  float roll = 0.0f;     // for CameraAim::LookAt: degrees, positive turns the right side down
  float fov = 0.0f;      // degrees; 0 keeps the game's own
  float seconds = 0.0f;  // blend from the current camera; 0 cuts
  bool ease = true;      // the move starts and ends slowly; false: one speed throughout
  // The mouse turns the view: look_yaw degrees to each side (0: a still shot, up to 180),
  // look_up and look_down from where the shot looks (up to 89).
  std::uint8_t look_yaw = 0;
  std::uint8_t look_up = 0;
  std::uint8_t look_down = 0;

  friend bool operator==(const CameraShot& a, const CameraShot& b) {
    return std::memcmp(a.position, b.position, sizeof(a.position)) == 0 && a.aim == b.aim &&
           std::memcmp(a.rotation, b.rotation, sizeof(a.rotation)) == 0 &&
           std::memcmp(a.target, b.target, sizeof(a.target)) == 0 && a.roll == b.roll &&
           a.fov == b.fov && a.seconds == b.seconds && a.ease == b.ease &&
           a.look_yaw == b.look_yaw && a.look_up == b.look_up && a.look_down == b.look_down;
  }
};

namespace camera_shot_detail {

inline void write_float(core::BitWriter& w, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  w.write_bits(bits, 32);
}

inline float read_float(core::BitReader& r) {
  const std::uint32_t bits = std::uint32_t(r.read_bits(32));
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline bool valid_point(const float point[3]) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(point[i]) || std::fabs(point[i]) > kMaxCameraCoordinate) {
      return false;
    }
  }
  return true;
}

}  // namespace camera_shot_detail

// A shot that can be filmed: a point in the world, a rotation of some length or a target away
// from the camera, a field of view of 0 or kMinCameraFov .. kMaxCameraFov and a blend of
// 0 .. kMaxCameraSeconds.
inline bool valid_camera_shot(const CameraShot& shot) {
  if (!camera_shot_detail::valid_point(shot.position)) {
    return false;
  }
  if (shot.aim == CameraAim::Rotation) {
    double length = 0;
    for (float c : shot.rotation) {
      if (!std::isfinite(c)) {
        return false;
      }
      length += double(c) * c;
    }
    if (length < 1e-6) {
      return false;
    }
  } else if (shot.aim == CameraAim::LookAt) {
    if (!camera_shot_detail::valid_point(shot.target)) {
      return false;
    }
    const float dx = shot.target[0] - shot.position[0];
    const float dy = shot.target[1] - shot.position[1];
    const float dz = shot.target[2] - shot.position[2];
    if (dx * dx + dy * dy + dz * dz < 1e-6f) {
      return false;
    }
    if (!std::isfinite(shot.roll) || std::fabs(shot.roll) > kMaxCameraRoll) {
      return false;
    }
  } else {
    return false;
  }
  const bool fov_ok = shot.fov == 0.0f || (shot.fov >= kMinCameraFov && shot.fov <= kMaxCameraFov);
  const bool look_ok = shot.look_yaw <= 180 && shot.look_up <= 89 && shot.look_down <= 89;
  return fov_ok && look_ok && std::isfinite(shot.seconds) && shot.seconds >= 0.0f &&
         shot.seconds <= kMaxCameraSeconds;
}

// CAMERA_SHOT; empty for a shot that is not valid_camera_shot.
inline std::vector<std::uint8_t> encode_camera_shot(const CameraShot& shot) {
  if (!valid_camera_shot(shot)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kCameraShotTag, 8);
  for (float c : shot.position) {
    camera_shot_detail::write_float(w, c);
  }
  w.write_bits(std::uint8_t(shot.aim), 8);
  if (shot.aim == CameraAim::Rotation) {
    for (float c : shot.rotation) {
      camera_shot_detail::write_float(w, c);
    }
  } else {
    for (float c : shot.target) {
      camera_shot_detail::write_float(w, c);
    }
    camera_shot_detail::write_float(w, shot.roll);
  }
  camera_shot_detail::write_float(w, shot.fov);
  camera_shot_detail::write_float(w, shot.seconds);
  w.write_bits(shot.ease ? 1 : 0, 8);
  if (shot.look_yaw > 0) {
    w.write_bits(shot.look_yaw, 8);
    w.write_bits(shot.look_up, 8);
    w.write_bits(shot.look_down, 8);
  }
  return w.bytes();
}

inline bool decode_camera_shot(const std::vector<std::uint8_t>& p, CameraShot& shot) {
  if (p.empty() || p[0] != kCameraShotTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  CameraShot decoded;
  for (float& c : decoded.position) {
    c = camera_shot_detail::read_float(r);
  }
  const std::uint8_t aim = std::uint8_t(r.read_bits(8));
  if (aim == std::uint8_t(CameraAim::Rotation)) {
    decoded.aim = CameraAim::Rotation;
    for (float& c : decoded.rotation) {
      c = camera_shot_detail::read_float(r);
    }
  } else if (aim == std::uint8_t(CameraAim::LookAt)) {
    decoded.aim = CameraAim::LookAt;
    for (float& c : decoded.target) {
      c = camera_shot_detail::read_float(r);
    }
    decoded.roll = camera_shot_detail::read_float(r);
  } else {
    return false;
  }
  decoded.fov = camera_shot_detail::read_float(r);
  decoded.seconds = camera_shot_detail::read_float(r);
  if (r.bits_remaining() >= 8) {
    const std::uint8_t ease = std::uint8_t(r.read_bits(8));
    if (ease > 1) {
      return false;
    }
    decoded.ease = ease == 1;
  }
  if (r.bits_remaining() >= 24) {
    decoded.look_yaw = std::uint8_t(r.read_bits(8));
    decoded.look_up = std::uint8_t(r.read_bits(8));
    decoded.look_down = std::uint8_t(r.read_bits(8));
    if (decoded.look_yaw == 0) {
      return false;
    }
  }
  if (r.overrun() || r.bits_remaining() >= 8 || !valid_camera_shot(decoded)) {
    return false;
  }
  shot = decoded;
  return true;
}

// CAMERA_CLEAR: [tag].
inline std::vector<std::uint8_t> encode_camera_clear() {
  return {kCameraClearTag};
}

inline bool decode_camera_clear(const std::vector<std::uint8_t>& p) {
  return p.size() == 1 && p[0] == kCameraClearTag;
}

// CAMERA_SHOT_ENDED: [tag][reason u8].
inline std::vector<std::uint8_t> encode_camera_shot_ended(CameraShotEnd reason) {
  if (std::uint8_t(reason) >= kCameraShotEndCount) {
    return {};
  }
  return {kCameraShotEndedTag, std::uint8_t(reason)};
}

inline bool decode_camera_shot_ended(const std::vector<std::uint8_t>& p, CameraShotEnd& reason) {
  if (p.size() != 2 || p[0] != kCameraShotEndedTag || p[1] >= kCameraShotEndCount) {
    return false;
  }
  reason = CameraShotEnd(p[1]);
  return true;
}

// The reason's name, as the resource host reads it.
inline const char* camera_shot_end_name(CameraShotEnd reason) {
  switch (reason) {
    case CameraShotEnd::WorldChanged:
      return "worldChanged";
    case CameraShotEnd::Unavailable:
      return "unavailable";
    case CameraShotEnd::Lost:
      return "lost";
  }
  return "lost";
}

}  // namespace kcd2s::integration
