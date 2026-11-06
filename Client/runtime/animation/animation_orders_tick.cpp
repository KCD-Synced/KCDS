// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "animation_orders_tick.hpp"

#include <mutex>
#include <vector>

#include "../attach/player_attach.hpp"
#include "../body_state/body_readings.hpp"
#include "../camera/camera_shot.hpp"
#include "../camera/free_look.hpp"
#include "../camera/third_person.hpp"
#include "../carts/placed_carts.hpp"
#include "../core/log.hpp"
#include "../noclip/free_camera.hpp"
#include "animation_props.hpp"
#include "kcd2s/integration/animation_props.hpp"
#include "no_gravity.hpp"
#include "pointing.hpp"
#include "player_animation.hpp"
#include "upper_body.hpp"

namespace kcds::game_client {

void tick_animation_orders(const Frame& frame) {
  const auto local_actor = frame.local_actor;
  const auto local_entity = frame.local_entity;
  // The server's Player.playAnimation and stopAnimation for the local player,
  // played by the game's Mannequin (animation/player_animation.hpp); each request's end the
  // client sees goes back to the server.
  std::vector<kcd2s::integration::AnimationOrder> orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      orders = client->peer().take_animation_orders();
    }
  }
  void* controller = kcds::runtime::body_state::action_controller(local_actor);
  std::vector<kcd2s::integration::AnimationEnd> ends;
  for (const auto& order : orders) {
    using Kind = kcd2s::integration::AnimationOrder::Kind;
    const char* kind = "stop";
    if (order.kind == Kind::Play) {
      kind = "play";
    } else if (order.kind == Kind::Face) {
      kind = "face";
    } else if (order.kind == Kind::Talk) {
      kind = "talk";
    } else if (order.kind == Kind::Speed) {
      kind = "speed";
    } else if (order.kind == Kind::Props) {
      kind = "props";
    }
    const bool play = order.kind == Kind::Play;
    log("ANIMATION order %s request=%llu fragment=%s tags=%s loop=%u lock=%u restore=%u "
        "stand_up=%u speed=%.2f start=%.2f",
        kind, static_cast<unsigned long long>(order.request), order.fragment.c_str(),
        order.tags.c_str(), unsigned(order.loop), unsigned(order.lock_movement),
        unsigned(order.restore), unsigned(order.stand_up), order.speed, order.start);
    if (play && (!order.clip.empty() || !order.props.empty())) {
      log("ANIMATION request=%llu clip=%s props=%s", static_cast<unsigned long long>(order.request),
          order.clip.c_str(), kcd2s::integration::animation_props_field(order.props).c_str());
    }
    kcds::runtime::player_animation::take(controller, order, ends);
  }
  kcds::runtime::player_animation::pause_talking(game_in_dialogue.load());
  // The pointing key: a request of the client's own while no request of the server's is
  // in place.
  kcds::runtime::animation::pointing::keep_local(controller, local_entity);
  kcds::runtime::player_animation::frame(controller, ends);
  // An upper-body request keeps the walk's legs under it (animation/upper_body.hpp).
  if (kcds::runtime::player_animation::upper_body_running()) {
    kcds::runtime::animation::upper_body::keep_legs(
        local_entity, kcds::runtime::player_animation::kScriptClipLayer);
  }
  // A request with lockRotation turns only the head and the camera, and so does a player
  // attached with look 'free'.
  const bool held_look = kcds::runtime::player_animation::lock_rotation_running() ||
                         kcds::runtime::attach::free_look_wanted();
  // So does a player on a cart's seat, who keeps the controls: a driver drives and anyone gets down
  // with them. In the third person their view goes round the body from behind.
  const bool seat_look = kcds::runtime::carts::placed::local_held();
  // A scripted shot and NoClip have the camera to themselves meanwhile.
  const bool camera_taken = kcds::runtime::camera::shot_active() || kcds::runtime::noclip::active();
  // In the third person their view goes round the body from behind; under a held look only when
  // a client script holds the third person (Camera.setThirdPerson: an exported studio clip shows
  // the player from behind, it showed the eyes instead).
  const bool behind = kcds::runtime::camera::third_person::in_third_person() &&
                      (!held_look || kcds::runtime::camera::third_person::script_view_on());
  // The request's own look limits (lockRotation { yaw, up, down }); none for the others.
  const auto limits = kcds::runtime::player_animation::look_limits();
  kcds::runtime::camera::free_look::set_limits(limits.yaw, limits.up, limits.down);
  kcds::runtime::camera::free_look::keep(local_entity, held_look || seat_look, true, held_look,
                                         behind, camera_taken);
  // A request without gravity keeps the body where the script put it.
  kcds::runtime::animation::no_gravity::keep(local_entity,
                                             kcds::runtime::player_animation::no_gravity_running());
  // The request's props on the player's own body, as every other client shows them from the
  // field "animationProps".
  kcds::runtime::animation_props::keep(
      kcds::runtime::animation_props::kLocalHolder, local_entity,
      kcd2s::integration::animation_props_field(kcds::runtime::player_animation::props()));
  // The moments the player's animation took a prop that follows it or put it down.
  for (const auto& own : kcds::runtime::animation_props::take_own_cues()) {
    kcd2s::integration::AnimationPropCue cue;
    cue.request = kcds::runtime::player_animation::running_request();
    cue.prop = own.prop;
    cue.taken = own.taken;
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client && cue.request != 0) {
      client->peer().send_animation_prop_cue(cue);
    }
  }
  for (const auto& end : ends) {
    // A request of the client's own (no id) reports to nobody.
    if (end.request == 0) {
      continue;
    }
    log("ANIMATION request=%llu ended reason=%u exit=%u (%s)",
        static_cast<unsigned long long>(end.request), unsigned(end.reason), unsigned(end.exit),
        kcds::runtime::player_animation::end_detail());
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      client->peer().send_animation_end(end);
    }
  }
}

}  // namespace kcds::game_client
