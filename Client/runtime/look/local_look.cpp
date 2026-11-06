#include "local_look.hpp"

#include "../character_creator/creator_stage.hpp"
#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../noclip/free_camera.hpp"
#include "../ranged/aim.hpp"
#include "../seats/seat_reservations.hpp"
#include "native_look.hpp"

#include <cmath>

namespace kcds::game_client {

// The view camera's world matrix (3x4, rows; the position in the last column, the view along
// its second column) as read this frame.
std::array<float, 12> view_matrix{};
bool view_matrix_valid = false;
float view_fov = 0.0f;
// Where the local player's head and eyes are turned (the field "gaze"): the
// unit vector from its eyes (the view camera, which in the game's own view sits in the head) to
// its look target, or along the view without one; to a thousandth, so a still head publishes
// nothing new. Kept while a free camera (NoClip, the CharacterCreator) films instead of the eyes.
core::Vec3 tick_look_direction(core::Vec3 look_target, bool look_valid) {
  static core::Vec3 last{};
  if (!view_matrix_valid || kcds::runtime::noclip::active() ||
      kcds::runtime::character_creator::active()) {
    return last;
  }
  const auto& m = view_matrix;
  core::Vec3 direction{m[1], m[5], m[9]};
  if (look_valid) {
    const core::Vec3 to_target{look_target.x - m[3], look_target.y - m[7], look_target.z - m[11]};
    if (to_target.x * to_target.x + to_target.y * to_target.y + to_target.z * to_target.z > 0.01f)
      direction = to_target;
  }
  const float length =
      std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
  if (!(length > 1e-6f) || !std::isfinite(length))
    return last;
  const auto thousandth = [&](float value) {
    return std::round(value / length * 1000.0f) / 1000.0f;
  };
  last = {thousandth(direction.x), thousandth(direction.y), thousandth(direction.z)};
  return last;
}
// Every 3 s: the local player's look and what its own native look IK makes of it in which posture
// the game's reference for the "LOOK replica" lines of its replicas on the other clients.
void log_local_look(void* look, void* skeleton, bool valid, bool enabled, std::int64_t posture) {
  static ULONGLONG next_log = 0;
  const ULONGLONG now_ms = GetTickCount64();
  if (now_ms < next_log) {
    return;
  }
  next_log = now_ms + 3000;
  const bool seated = !kcds::runtime::seats::local_seats().empty();
  const NativeLookUse use = native_look_use(look, skeleton);
  log("LOOK local posture=%s valid=%u enabled=%u state=%u layer=%u look_pose=%d poses=%u "
      "fade=%.2f",
      look_posture_name(local_rider_seated.load(), seated, posture), unsigned(valid),
      unsigned(enabled), unsigned(use.state), use.layer, use.clip, use.poses, use.fade);
}

void read_local_look(void* local_entity, LocalLookReading& reading) {
  void* local_look = nullptr;
  void* local_aim = nullptr;
  if (auto character =
          slot<void*(__fastcall*)(void*, unsigned)>(local_entity, 101)(local_entity, 0)) {
    local_skeleton.store(slot<void*(__fastcall*)(void*)>(character, 5)(character));
    local_look = native_look(character);
    local_aim = kcds::runtime::ranged::aim_of(native_pose(character));
  }
  core::Vec3 look_target{}, look_tuning{};
  bool look_enabled = false, look_valid = false;
  if (local_look) {
    look_target = *reinterpret_cast<core::Vec3*>(static_cast<std::uint8_t*>(local_look) + 144);
    look_enabled =
        *reinterpret_cast<std::uint8_t*>(static_cast<std::uint8_t*>(local_look) + 140) != 0;
    auto fade = *reinterpret_cast<core::Vec3*>(static_cast<std::uint8_t*>(local_look) + 160);
    look_tuning = {std::isfinite(fade.x) && fade.x > 0 ? 1.0f / fade.x : 0,
                   std::isfinite(fade.y) && fade.y > 0 ? 1.0f / fade.y : 0, 0};
    look_valid = finite_look(look_target);
  }
  reading.local_look = local_look;
  reading.local_aim = local_aim;
  reading.look_target = look_target;
  reading.look_tuning = look_tuning;
  reading.look_enabled = look_enabled;
  reading.look_valid = look_valid;
}

}  // namespace kcds::game_client
