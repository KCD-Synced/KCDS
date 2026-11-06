#include "cursor_pick.hpp"

#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>

#include "../chat/chat_overlay.hpp"
#include "../core/log.hpp"
#include "../physics/local_world.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace kcds::runtime::architect::cursor_pick {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr float kStageWidth = 1920.0f;
constexpr float kStageHeight = 1080.0f;
constexpr float kReach = 500.0f;

std::atomic<bool> following{false};
std::mutex newest_mutex;
std::optional<sdk::Value> newest;

// What was sent last: the cursor in client pixels and the camera, so an unchanged frame sends
// nothing.
struct Seen {
  long x = -1;
  long y = -1;
  float camera[12] = {};
};
Seen seen;
// Set by a start or stop (any thread): the next frame sends whatever it finds.
std::atomic<bool> reset_seen{false};

bool same_camera(const float a[12], const float b[12]) {
  for (int i = 0; i < 12; ++i) {
    if (std::fabs(a[i] - b[i]) > 1e-4f) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "architectCursor") {
    return false;
  }
  const sdk::Value* on = integration::script_field(message, "on");
  following.store(local_mode::enabled() && on && on->tag() == sdk::Tag::Bool && on->as_bool());
  reset_seen.store(true);
  return true;
}

void frame(bool in_world) {
  if (!in_world || !following.load()) {
    return;
  }
  if (reset_seen.exchange(false)) {
    seen = Seen{};
  }
  HWND window = static_cast<HWND>(kcd2s::native_chat::window());
  POINT cursor{};
  RECT client{};
  if (!window || !GetCursorPos(&cursor) || !ScreenToClient(window, &cursor) ||
      !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) {
    return;
  }
  float camera[12];
  float fov = 0.0f;
  if (!physics::view_camera(camera, fov) || !(fov > 0.0f)) {
    return;
  }
  if (cursor.x == seen.x && cursor.y == seen.y && same_camera(camera, seen.camera)) {
    return;
  }
  seen.x = cursor.x;
  seen.y = cursor.y;
  std::copy(camera, camera + 12, seen.camera);
  const float width = float(client.right);
  const float height = float(client.bottom);
  const bool over =
      cursor.x >= 0 && cursor.y >= 0 && cursor.x < client.right && cursor.y < client.bottom;
  // The camera fills the window: its point in -1 .. 1, up positive.
  const float nx = float(cursor.x) / width * 2.0f - 1.0f;
  const float ny = 1.0f - float(cursor.y) / height * 2.0f;
  const float tan_v = std::tan(fov / 2.0f);
  const float tan_h = tan_v * (width / height);
  float direction[3];
  float length = 0.0f;
  for (int axis = 0; axis < 3; ++axis) {
    const float right = camera[axis * 4 + 0];
    const float forward = camera[axis * 4 + 1];
    const float up = camera[axis * 4 + 2];
    direction[axis] = forward + right * nx * tan_h + up * ny * tan_v;
    length += direction[axis] * direction[axis];
  }
  length = std::sqrt(length);
  sdk::Value hit;
  if (over && length > 0.0f) {
    const float from[3] = {camera[3], camera[7], camera[11]};
    const float to[3] = {from[0] + direction[0] / length * kReach,
                         from[1] + direction[1] / length * kReach,
                         from[2] + direction[2] / length * kReach};
    hit = physics::first_hit(from, to);
  }
  // Test builds: what the cursor came to, every two seconds.
  static const bool testing = GetEnvironmentVariableW(L"KCDS_TEST_HOOKS", nullptr, 0) != 0;
  static DWORD last_log = 0;
  if (testing && GetTickCount() - last_log > 2000) {
    last_log = GetTickCount();
    kcds::game_client::log("ARCHITECT CURSOR client %ld %ld of %ld x %ld over=%d hit=%d", cursor.x,
                           cursor.y, client.right, client.bottom, over ? 1 : 0,
                           hit.tag() == sdk::Tag::Map ? 1 : 0);
  }
  sdk::Value message(sdk::Value::Map{
      {"op", sdk::Value(std::string("architectCursorAt"))},
      {"x", sdk::Value(double(float(cursor.x) / width * kStageWidth))},
      {"y", sdk::Value(double(float(cursor.y) / height * kStageHeight))},
      {"over", sdk::Value(over)},
      {"hit", std::move(hit)},
      // The editor lays itself out on a stage of the window's shape (theme.cjs stageFor).
      {"window",
       sdk::Value(sdk::Value::Array{sdk::Value(double(width)), sdk::Value(double(height))})},
  });
  std::lock_guard<std::mutex> lock(newest_mutex);
  newest = std::move(message);
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::optional<sdk::Value> taken;
  {
    std::lock_guard<std::mutex> lock(newest_mutex);
    taken.swap(newest);
  }
  if (taken) {
    send(*taken);
  }
}

}  // namespace kcds::runtime::architect::cursor_pick
