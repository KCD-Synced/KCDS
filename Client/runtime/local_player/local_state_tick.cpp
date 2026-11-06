// Winsock before Windows.h (see client_internal.hpp).
#include "../animation/hand_props.hpp"
#include "../core/client_internal.hpp"

#include "local_state_tick.hpp"

#include "../architect/local_level_edits.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../animation/animation_capture.hpp"
#include "../animation/player_animation.hpp"
#include "../animation/pointing.hpp"
#include "../camera/third_person.hpp"
#include "../camera/camera_shot.hpp"
#include "../noclip/free_camera.hpp"
#include "../appearance/appearance_tick.hpp"
#include "../appearance/components.hpp"
#include "../appearance/first_person_head.hpp"
#include "../appearance/grime.hpp"
#include "../appearance/outfit.hpp"
#include "../body_state/body_state_tick.hpp"
#include "../book/resource_book.hpp"
#include "../branding/runtime_branding.hpp"
#include "../buffs/buffs_tick.hpp"
#include "../carry/carry_items.hpp"
#include "../carry/carry_orders.hpp"
#include "../combat/combat_identity.hpp"
#include "../combat/melee_contacts.hpp"
#include "../combat/native_actions.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../crafting/alchemy.hpp"
#include "../crafting/grinding.hpp"
#include "../crafting/smithing.hpp"
#include "../gates/gates.hpp"
#include "../level/level_edits.hpp"
#include "../look/local_look.hpp"
#include "../progression/progression_report.hpp"
#include "../progression/progression_tick.hpp"
#include "../ranged/aim.hpp"
#include "../ranged/shots.hpp"
#include "../replicas/forced_transforms.hpp"
#include "../replicas/motion_capture.hpp"
#include "../replicas/replica_removal.hpp"
#include "../replicas/replica_spawn.hpp"
#include "../riding/native_horses.hpp"
#include "../riding/trample.hpp"
#include "../seats/seat_reservations.hpp"
#include "../stash/stashes.hpp"
#include "../stats/stats_tick.hpp"
#include "../weapons/drawn_weapons.hpp"
#include "../weapons/held_weapon.hpp"
#include "kcd2s/integration/animation_props.hpp"

namespace kcds::game_client {

namespace integration = kcd2s::integration;
namespace replication = kcd2s::replication;

void send_local_state(const Frame& frame, bool& sent) {
  // The frame's values under the names frame() gave them.
  const auto now = frame.now;
  const auto own = frame.own;
  const auto entity_system = frame.entity_system;
  const auto& world = frame.world;
  const auto local_actor = frame.local_actor;
  const auto local_entity = frame.local_entity;
  const auto local_id = frame.local_id;
  const auto local_look = frame.local_look;
  const auto local_aim = frame.local_aim;
  const auto& look_target = frame.look_target;
  const auto& look_tuning = frame.look_tuning;
  const auto look_enabled = frame.look_enabled;
  const auto look_valid = frame.look_valid;
  // Teleports must be applied before the local pose is captured below; see apply_forced_transforms.
  const bool teleported =
      apply_forced_transforms(entity_system, local_entity, local_id, own, world.revision);
  const float* tm = slot<const float*(__fastcall*)(void*)>(local_entity, 32)(local_entity);
  core::Vec3 p{tm[3], tm[7], tm[11]};
  core::Quat q = rotation(tm);
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
    return;
  static core::Vec3 previous;
  static double previous_time = 0;
  if (teleported) {
    // A teleport is not movement; do not report the jump as velocity.
    previous_time = 0;
  }
  core::Vec3 velocity{};
  double dt = now - previous_time;
  if (previous_time && dt > 0 && dt < 0.25) {
    velocity = {float((p.x - previous.x) / dt), float((p.y - previous.y) / dt),
                float((p.z - previous.z) / dt)};
  }
  previous = p;
  previous_time = now;
  NativeMovementRequest motion;
  bool valid;
  std::uint32_t mask;
  float params[18];
  std::string animations;
  std::string human_overlay;
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    motion = local_motion;
    valid = local_motion_valid;
    mask = motion_mask;
    std::memcpy(params, motion_params, sizeof(params));
    animations = local::encode_animation(local_animations);
    if (human_overlay_ready.load()) {
      human_overlay = local::encode_human_overlay(local_human_overlay);
    }
  }
  // Observe the actual local outcome after the pair exits, including frames
  // where no new button event arrives. Original input/animation calls unchanged.
  auto foot_input =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) + 2952);
  const auto observation_tick = GetTickCount64();
  static ULONGLONG next_pipeline = 0;
  DWORD foreground_pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
  const bool physical_move =
      foreground_pid == GetCurrentProcessId() && ((GetAsyncKeyState('W') | GetAsyncKeyState('A') |
                                                   GetAsyncKeyState('S') | GetAsyncKeyState('D')) &
                                                  0x8000) != 0;
  const bool raw_move = foot_input && (*reinterpret_cast<float*>(foot_input + 88) != 0.f ||
                                       *reinterpret_cast<float*>(foot_input + 92) != 0.f);
  if (foot_input && observation_tick >= next_pipeline &&
      (physical_move || raw_move ||
       (last_local_pair_exit && observation_tick - last_local_pair_exit < 10000))) {
    next_pipeline = observation_tick + 100;
    auto movement =
        *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) + 632);
    auto native_core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
    auto model = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) + 2448);
    auto controller = movement ? *reinterpret_cast<std::uint8_t**>(movement + 1104) : nullptr;
    auto filter = controller ? *reinterpret_cast<std::uint8_t**>(controller + 56) : nullptr;
    const auto flags = *reinterpret_cast<std::uint64_t*>(foot_input + 224);
    log("CONTROL pipeline action=%u state=%u clinch=%u physical=%u foreground=%lu raw=(%.3f %.3f) "
        "filtered=(%.3f %.3f) gate=%u filter=%u request_flags=%llx request=(%.3f %.3f %.3f) "
        "actual=(%.3f %.3f %.3f) position=(%.3f %.3f %.3f) tick=%llu",
        native_action_type(movement),
        native_core ? *reinterpret_cast<unsigned*>(native_core + 56) : 0u,
        native_core ? *reinterpret_cast<unsigned*>(native_core + 104) : 0u, unsigned(physical_move),
        foreground_pid, *reinterpret_cast<float*>(foot_input + 88),
        *reinterpret_cast<float*>(foot_input + 92), *reinterpret_cast<float*>(foot_input + 100),
        *reinterpret_cast<float*>(foot_input + 104), model ? unsigned(model[965]) : 0u,
        filter ? unsigned(filter[22]) : 0u, flags, valid ? motion.velocity.x : 0.f,
        valid ? motion.velocity.y : 0.f, valid ? motion.velocity.z : 0.f, velocity.x, velocity.y,
        velocity.z, p.x, p.y, p.z, observation_tick);
    if (auto skeleton = local_skeleton.load())
      if ((*reinterpret_cast<void***>(skeleton))[28] ==
          reinterpret_cast<void*>(native_animation_progress)) {
        const auto count = slot<unsigned(__fastcall*)(void*, unsigned)>(skeleton, 15)(skeleton, 0);
        if (count <= 16)
          for (unsigned i = 0; i < count; ++i) {
            auto entry = slot<const std::uint8_t*(__fastcall*)(void*, unsigned, unsigned)>(
                skeleton, 18)(skeleton, 0, i);
            if (entry)
              log("CONTROL pipeline root entry=%u clip=%d progress=%.4f flags=%x token=%u "
                  "user_token=%u tick=%llu",
                  i, *reinterpret_cast<const int*>(entry + 16),
                  native_animation_progress(skeleton, entry),
                  *reinterpret_cast<const unsigned*>(entry + 76),
                  *reinterpret_cast<const unsigned*>(entry + 48),
                  *reinterpret_cast<const unsigned*>(entry + 84), observation_tick);
          }
      }
  }
  std::vector<std::pair<core::FieldId, replication::FieldValue>> input{
      {local::Position, p},
      {local::Rotation, q},
      {local::Velocity, velocity},
      {local::PoseValid, true},
      {local::LookTuning, look_tuning},
      {local::LookTarget, look_target},
      {local::LookEnabled, look_enabled},
      {local::LookValid, look_valid},
      {local::NativeRiderState, capture_native_rider_state(local_actor)},
      {local::Animations, animations},
      {local::HumanOverlayAnimation, human_overlay},
      {local::RagdollPose, capture_ragdoll_pose(local_entity)},
      {local::MotionValid, valid},
      {local::MotionType, std::int64_t(motion.type)},
      {local::MotionVelocity, motion.velocity},
      {local::MotionRotation, motion.rotation},
      {local::MotionFlags, std::int64_t((motion.allow_strafe ? 1 : 0) | (motion.jumping ? 2 : 0))},
      {local::MotionMask, std::int64_t(mask)}};
  const bool ui_blocking = local_full_screen_ui();
  if (ui_blocking)
    local_ui_blocking_ms.store(GetTickCount64());
  static bool previous_ui_blocking = false;
  if (ui_blocking && !previous_ui_blocking) {
    // Opening a full-screen UI while locked on an opponent left the lock-on pair animations
    // running underneath (live report). Release the lock the way the engine does when it loses
    // its target: SetCombatTarget with no entity. relay_native_target sends the unlock.
    auto movement =
        *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) + 632);
    auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
    if (core && *reinterpret_cast<void**>(core + 3320)) {
      hooked_set_combat_target(movement, nullptr);
      log("COMBAT TARGET released for full-screen UI");
    }
  }
  previous_ui_blocking = ui_blocking;
  input.emplace_back(local::UiBlocking, ui_blocking);
  input.emplace_back(local::HeldProp, kcds::runtime::book::held_prop());
  // What the player does at a smithy, its tools and workpiece where its game holds them,
  // at an alchemy table, the table's things as its game shows them, or at a grindstone
  // its wheel, sparks and sound.
  std::string station_action = kcds::runtime::crafting::smithing::own_field(
      local_actor, local_entity, local::clock_seconds());
  {
    const std::string alchemy_action = kcds::runtime::crafting::alchemy::own_field(
        local_actor, local_entity, local::clock_seconds());
    if (station_action.empty()) {
      station_action = alchemy_action;
    }
    const std::string grinding_action = kcds::runtime::crafting::grinding::own_field(
        local_actor, local_entity, local::clock_seconds());
    if (station_action.empty()) {
      station_action = grinding_action;
    }
  }
  // The talking style first, then the shipped clip and the props: the script
  // request's, then the tools the game itself put in the player's hands, unless a station
  // shows its own.
  {
    auto props = kcds::runtime::player_animation::props();
    if (station_action.empty()) {
      for (auto& prop : kcds::runtime::animation::hand_props::props()) {
        if (props.size() < kcd2s::integration::kAnimationPropsMax) {
          props.push_back(std::move(prop));
        }
      }
    }
    // The request's speed leads, then the point the arm points at, then an
    // upper-body request's mark, so every other client keeps the legs.
    const std::string pointing =
        kcds::runtime::animation::pointing::local_mark().value_or(std::string());
    input.emplace_back(
        local::AnimationProps,
        kcd2s::integration::speed_field(
            kcds::runtime::player_animation::speed_mark(),
            kcd2s::integration::pointing_field(
                pointing, kcd2s::integration::upper_body_field(
                              kcds::runtime::player_animation::upper_body_running(),
                              kcd2s::integration::talking_field(
                                  kcds::runtime::player_animation::talking_style(),
                                  kcd2s::integration::animation_field(
                                      kcds::runtime::player_animation::shipped_clip(), props))))));
  }
  input.emplace_back(local::StationAction, std::move(station_action));
  {
    std::string outfit = kcds::runtime::appearance::outfit::capture(local_actor);
    // A body being rebuilt (a respawn, a skeleton change, a world load) can show no worn pieces for
    // a moment; published, it stripped the player on every other screen for good. An
    // empty capture after pieces is held back until it has lasted kEmptyOutfitHold seconds.
    static std::string last_dressed;
    static double empty_since = 0;
    constexpr double kEmptyOutfitHold = 3.0;
    constexpr std::size_t kEmptyOutfitSize = 3;  // the tag alone
    if (outfit.size() > kEmptyOutfitSize) {
      last_dressed = outfit;
      empty_since = 0;
    } else if (!last_dressed.empty()) {
      if (empty_since == 0) {
        empty_since = now;
      }
      if (now - empty_since < kEmptyOutfitHold) {
        outfit = last_dressed;
      } else {
        last_dressed.clear();
      }
    }
    static std::string reported;
    if (outfit != reported) {
      reported = outfit;
      log("APPEARANCE outfit own %s", kcds::runtime::appearance::outfit::describe(outfit).c_str());
    }
    input.emplace_back(local::Outfit, std::move(outfit));
  }
  {
    // Dirt, blood and wear: they change slowly, so a fresh capture every 2 s, and at once
    // after a server's order to clean or soil the player.
    static std::string grime;
    static double next_grime = 0;
    std::vector<kcd2s::integration::GrimeOrder> grime_orders;
    {
      std::lock_guard<std::mutex> lock(network_mutex);
      grime_orders = client->peer().take_grime_orders();
    }
    for (const auto& order : grime_orders) {
      kcds::runtime::appearance::grime::order(local_actor, order);
      next_grime = 0;
    }
    if (now >= next_grime) {
      next_grime = now + 2;
      grime = kcds::runtime::appearance::grime::capture(local_actor);
    }
    input.emplace_back(local::Grime, grime);
  }
  {
    // The server's level edits: level entities and brushes hidden or moved; and its
    // gates, whose static meshes are level edits of their own.
    std::vector<kcd2s::integration::LevelEditChange> level_edits;
    std::vector<kcd2s::integration::GateState> gate_states;
    std::string map;
    {
      std::lock_guard<std::mutex> lock(network_mutex);
      level_edits = client->peer().take_level_edit_changes();
      gate_states = client->peer().take_gate_states();
      const auto& world = client->peer().world_config();
      map = world ? world->map : std::string();
    }
    kcds::runtime::gates::frame(map, gate_states, level_edits);
    // World Architect's own, in its local mode (architect/local_level_edits.hpp).
    for (auto& change : kcds::runtime::architect::level_edits::take()) {
      level_edits.push_back(std::move(change));
    }
    kcds::runtime::level::frame(level_edits);
  }
  tick_appearance_requests(local_actor);
  const auto worn = kcds::runtime::appearance::components::worn(local_actor);
  // The body's own head out of the first-person view, whatever face it has, also
  // while a script animation moves it away from the camera.
  const bool animating = kcds::runtime::player_animation::running_request() != 0 &&
                         !kcds::runtime::noclip::active() && !kcds::runtime::camera::shot_active();
  kcds::runtime::camera::third_person::set_script_animation(animating);
  kcds::runtime::stash::aim_chests(view_matrix_valid ? view_matrix.data() : nullptr);
  kcds::runtime::appearance::first_person_head::keep(
      local_actor, local_entity, view_matrix_valid ? view_matrix.data() : nullptr, animating, log);
  input.emplace_back(local::Appearance, kcd2s::integration::appearance_field(worn));
  input.emplace_back(local::BodyParts,
                     kcd2s::integration::body_parts_field(
                         kcds::runtime::appearance::components::built_from(local_actor)));
  input.emplace_back(local::BodyVisible, tick_body_visible(local_entity));
  // Whether the helmet's visor is down (Player.visorDown).
  input.emplace_back(local::VisorClosed,
                     kcds::runtime::appearance::outfit::visor_closed(local_actor));
  input.emplace_back(local::Disguise, tick_disguise(local_entity));
  tick_worn_only(local_actor, local_entity);
  input.emplace_back(local::Seats, kcds::runtime::seats::local_seats());
  tick_carry_orders(local_actor, p);
  // Test hook: where this game has its own player, for the desync check (desync487.py).
  static const bool pose_wanted =
      GetEnvironmentVariableW(L"KCDS_TEST_CHARACTER_METRICS", nullptr, 0) > 0;
  if (pose_wanted) {
    LARGE_INTEGER clock;
    LARGE_INTEGER rate;
    QueryPerformanceCounter(&clock);
    QueryPerformanceFrequency(&rate);
    log("POSE own %.3f %.3f %.3f %.3f", double(clock.QuadPart) / rate.QuadPart, p.x, p.y, p.z);
  }
  trace_lying_body("own", local_network_id.load(), local_entity);
  {
    // While the player carries an item, its hands hold only that, which the carry shows on the
    // replicas; the weapons sync would draw a second one.
    std::string carried = kcds::runtime::carry::capture(local_actor);
    input.emplace_back(local::Hands, carried.empty() ? kcds::runtime::weapons::capture(local_actor)
                                                     : std::string());
    input.emplace_back(local::Carry, std::move(carried));
  }
  {
    // The weapon in the hands and the clips its game started on it; a new weapon starts
    // with none.
    void* weapon = held_weapon_skeleton(local_actor, entity_system);
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    if (weapon != local_weapon_skeleton.load()) {
      local_weapon_skeleton.store(weapon);
      local_weapon_animations = local::AnimationState{};
    }
    input.emplace_back(local::WeaponAnimations,
                       weapon ? local::encode_animation(local_weapon_animations) : std::string());
  }
  {
    // Where the drawn weapon points, up and down included.
    const auto aim = kcds::runtime::ranged::read_aim(local_aim);
    input.emplace_back(local::AimTarget, aim.target);
    input.emplace_back(local::AimEnabled, aim.enabled);
    const std::uint8_t weapon = kcds::runtime::ranged::weapon_state(local_actor);
    static std::uint8_t was_weapon = 0;
    if (weapon != was_weapon) {
      was_weapon = weapon;
      log("SHOT local weapon state %u", unsigned(weapon));
    }
    input.emplace_back(local::WeaponState, std::int64_t(weapon));
    static double next_aim_log = 0;
    if (aim.enabled && now > next_aim_log) {
      next_aim_log = now + 1;
      log("AIM local target=(%.2f %.2f %.2f)", aim.target.x, aim.target.y, aim.target.z);
    }
  }
  {
    const auto& stats = tick_stats(local_actor);
    input.emplace_back(local::Vitals, stats.vitals);
    input.emplace_back(local::Stats, stats.stats);
    input.emplace_back(local::Abilities, stats.abilities);
    input.emplace_back(local::ReplicaSoul, tick_replica_soul(local_actor));
    input.emplace_back(local::ReplicaBuffs, tick_replica_buffs(local_actor));
    // A head look that is off keeps its last target (a point 10 m behind a player who had
    // turned to fight), so only an enabled one names where the eyes are.
    input.emplace_back(local::LookDirection,
                       tick_look_direction(look_target, look_valid && look_enabled));
    const std::int64_t posture = tick_posture(local_actor);
    input.emplace_back(local::Posture, posture);
    log_local_look(local_look, local_skeleton.load(), look_valid, look_enabled, posture);
    input.emplace_back(local::MotionClock,
                       std::int64_t(std::llround(local::clock_seconds() * 1000.0)));
    // The game's language, read again every few seconds: the player can change it.
    static std::string language;
    static double next_language_read = 0;
    if (now >= next_language_read) {
      next_language_read = now + 5;
      language = kcds::runtime::branding::cvar_value("g_language");
    }
    input.emplace_back(local::Language, language);
  }
  for (unsigned i = 0; i < 6; ++i)
    input.emplace_back(core::FieldId(local::MotionParams0 + i),
                       core::Vec3{params[i * 3], params[i * 3 + 1], params[i * 3 + 2]});
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_input(own, input);
  }
  sent = true;
}

}  // namespace kcds::game_client
