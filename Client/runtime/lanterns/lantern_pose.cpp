#include "lantern_pose.hpp"

#include "../core/client_internal.hpp"

#include <atomic>
#include <cstdint>

#include "../animation/player_animation.hpp"
#include "../body_state/body_readings.hpp"
#include "../core/log.hpp"
#include "../weapons/weapon_set.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::lanterns::pose {
namespace {

namespace integration = kcd2s::integration;

// The hold plays as a looped request of its own on the body's action controller, beside the
// scripts' requests (player_animation's bodies, keyed by an id no NPC has).
constexpr std::uint32_t kLanternBody = 0xFFFFFF01u;
constexpr char kFragment[] = "HoldItemLeft";
constexpr char kTags[] = "l_lamp";
// How long after a refused or cut hold it is tried again.
constexpr ULONGLONG kRetryMs = 2000;

std::atomic<bool> wanted{false};
// The torch in the left hand as the frame saw it, and as last sent (-1: not yet).
std::atomic<int> torch_seen{-1};
int torch_sent = -1;
bool holding = false;
void* held_on = nullptr;
ULONGLONG next_try = 0;

void stop_holding(const char* why) {
  if (!holding) {
    return;
  }
  player_animation::stop_on_body(kLanternBody);
  holding = false;
  held_on = nullptr;
  game_client::log("LANTERN hold ends (%s)", why);
}

}  // namespace

bool handle_runtime_message(const kcd2s::sdk::Value& message) {
  if (integration::script_operation(message) != "lanternPose") {
    return false;
  }
  const kcd2s::sdk::Value* pose = integration::script_field(message, "pose");
  wanted.store(pose && pose->tag() == kcd2s::sdk::Tag::Bool && pose->as_bool());
  return true;
}

void frame(void* local_actor, bool riding) {
  const bool torch = local_actor && weapons::set::torch_drawn(local_actor);
  if (local_actor) {
    torch_seen.store(torch ? 1 : 0);
  }
  void* controller = local_actor ? body_state::action_controller(local_actor) : nullptr;
  if (!wanted.load() || riding || torch || !controller) {
    stop_holding(riding ? "riding" : torch ? "torch" : "no lantern");
    return;
  }
  if (holding && held_on == controller) {
    // A loop is queued again pass after pass; an end comes back only when the game refused it.
    if (const auto ended = player_animation::watch_body(kLanternBody, controller)) {
      holding = false;
      held_on = nullptr;
      next_try = GetTickCount64() + kRetryMs;
      game_client::log("LANTERN hold refused by the game (%d)", int(*ended));
    }
    return;
  }
  const ULONGLONG now = GetTickCount64();
  if (now < next_try) {
    return;
  }
  next_try = now + kRetryMs;
  holding = player_animation::play_on_body(kLanternBody, controller, kFragment, kTags, true, 0);
  held_on = holding ? controller : nullptr;
  game_client::log("LANTERN hold %s", holding ? "plays" : "refused");
}

void pump(const RuntimeSender& send) {
  const int seen = torch_seen.load();
  if (seen < 0 || seen == torch_sent) {
    return;
  }
  kcd2s::sdk::Value::Map change{{"op", kcd2s::sdk::Value(std::string("lanternTorch"))},
                                {"torch", kcd2s::sdk::Value(seen == 1)}};
  if (send(kcd2s::sdk::Value(std::move(change)))) {
    torch_sent = seen;
    game_client::log("LANTERN torch in the left hand: %s", seen == 1 ? "drawn" : "put away");
  }
}

}  // namespace kcds::runtime::lanterns::pose
