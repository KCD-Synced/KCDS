#include "body_state_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../knockout/knockout.hpp"
#include "../npcs/native_npcs.hpp"
#include "../replicas/remote.hpp"
#include "../seats/seat_reservations.hpp"
#include "body_readings.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace kcds::game_client {
namespace {

// What went to a resource runtime that ended goes to the one started anew: forgets
// what was sent when the runtime is another than last time. network_mutex held.
template <class Sent>
void forget_on_new_runtime(std::uint32_t& runtime, Sent& sent) {
  const std::uint32_t current = client->resource_runtime_generation();
  if (current != runtime) {
    runtime = current;
    sent.clear();
  }
}

// Whether a player's replica holds a seat: its owner's field "seats" names one. network_mutex
// held.
bool replica_seated(core::EntityId id) {
  const auto& entities = client->peer().mirror().entities();
  const auto found = entities.find(id);
  if (found == entities.end() || found->second.fields.size() <= local::Seats) {
    return false;
  }
  const auto* seats = std::get_if<std::string>(&found->second.fields[local::Seats]);
  return seats && !seats->empty();
}

}  // namespace

// The engine entities of the players' bodies on this machine (Player.entityId): the
// local player's own and every replica's, by network id, to the resource runtime when they
// changed.
void tick_engine_ids() {
  namespace sdk = kcd2s::sdk;
  static std::vector<std::pair<core::EntityId, std::uint32_t>> sent;
  static std::uint32_t runtime = 0;
  std::vector<std::pair<core::EntityId, std::uint32_t>> ids;
  for (const auto& [id, remote] : remotes) {
    if (remote.engine_id)
      ids.emplace_back(id, remote.engine_id);
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  forget_on_new_runtime(runtime, sent);
  const auto own = client->own_avatar();
  if (own != core::kInvalidEntity && local_engine_id.load())
    ids.emplace_back(own, local_engine_id.load());
  std::sort(ids.begin(), ids.end());
  if (ids == sent || !client->resource_runtime_ready())
    return;
  sdk::Value::Array list;
  for (const auto& [id, engine] : ids) {
    list.push_back(sdk::Value(
        sdk::Value::Array{sdk::Value(std::int64_t(id)), sdk::Value(std::int64_t(engine))}));
  }
  if (client->send_to_resource_runtime(sdk::Value(sdk::Value::Map{
          {"op", sdk::Value(std::string("engineIds"))}, {"ids", sdk::Value(std::move(list))}})))
    sent = std::move(ids);
}

void tick_npc_bodies() {
  namespace sdk = kcd2s::sdk;
  constexpr ULONGLONG kPeriodMs = 250;
  constexpr float kMoved = 0.25f;
  static ULONGLONG read_at = 0;
  static std::vector<NpcBody> sent;
  static std::uint32_t runtime = 0;
  const ULONGLONG now = GetTickCount64();
  if (now - read_at < kPeriodMs) {
    return;
  }
  read_at = now;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    forget_on_new_runtime(runtime, sent);
  }
  const std::vector<NpcBody>& bodies = npc_bodies();
  bool changed = bodies.size() != sent.size();
  for (std::size_t i = 0; !changed && i < bodies.size(); ++i) {
    const NpcBody& now_body = bodies[i];
    const NpcBody& old_body = sent[i];
    const float dx = now_body.position.x - old_body.position.x;
    const float dy = now_body.position.y - old_body.position.y;
    const float dz = now_body.position.z - old_body.position.z;
    changed = now_body.id != old_body.id || now_body.engine_id != old_body.engine_id ||
              now_body.name != old_body.name || dx * dx + dy * dy + dz * dz > kMoved * kMoved;
  }
  if (!changed) {
    return;
  }
  sdk::Value::Array list;
  for (const NpcBody& body : bodies) {
    list.push_back(sdk::Value(sdk::Value::Array{
        sdk::Value(std::int64_t(body.id)), sdk::Value(std::int64_t(body.engine_id)),
        sdk::Value(double(body.position.x)), sdk::Value(double(body.position.y)),
        sdk::Value(double(body.position.z)), sdk::Value(body.name)}));
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  if (!client->resource_runtime_ready()) {
    return;
  }
  if (client->send_to_resource_runtime(sdk::Value(sdk::Value::Map{
          {"op", sdk::Value(std::string("npcBodies"))}, {"npcs", sdk::Value(std::move(list))}}))) {
    sent = bodies;
  }
}

// The Mannequin tags this machine's animation picked for the players' bodies (Player.movePace,
// moveDirection and bodyStance): the local player's and every replica's, by network id,
// to the resource runtime when they changed; read every 100 ms.
void tick_body_tags(void* local_actor) {
  namespace sdk = kcd2s::sdk;
  namespace body = kcds::runtime::body_state;
  constexpr ULONGLONG kBodyTagsPeriodMs = 100;
  static ULONGLONG read_at = 0;
  static std::vector<std::pair<core::EntityId, body::BodyTags>> sent;
  static std::uint32_t runtime = 0;
  const ULONGLONG now = GetTickCount64();
  if (now - read_at < kBodyTagsPeriodMs)
    return;
  read_at = now;
  std::vector<std::pair<core::EntityId, body::BodyTags>> tags;
  std::lock_guard<std::mutex> lock(network_mutex);
  forget_on_new_runtime(runtime, sent);
  for (const auto& [id, remote] : remotes) {
    if (remote.actor)
      tags.emplace_back(id, body::read_tags(remote.actor, replica_seated(id)));
  }
  const auto own = client->own_avatar();
  if (own != core::kInvalidEntity && local_actor) {
    const bool seated = !kcds::runtime::seats::local_seats().empty();
    tags.emplace_back(own, body::read_tags(local_actor, seated));
  }
  std::sort(tags.begin(), tags.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  if (tags == sent || !client->resource_runtime_ready())
    return;
  sdk::Value::Array list;
  for (const auto& [id, body_tags] : tags) {
    list.push_back(sdk::Value(sdk::Value::Array{
        sdk::Value(std::int64_t(id)), sdk::Value(std::int64_t(body_tags.move_speed)),
        sdk::Value(std::int64_t(body_tags.move_dir)), sdk::Value(std::int64_t(body_tags.stance))}));
  }
  if (client->send_to_resource_runtime(sdk::Value(sdk::Value::Map{
          {"op", sdk::Value(std::string("bodyTags"))}, {"tags", sdk::Value(std::move(list))}})))
    sent = std::move(tags);
}

// How the local player's body stands (the field "posture"): off the ground, crouched
// and its physics profile, as its own game reports them. Logged when it changes.
std::int64_t tick_posture(void* local_actor) {
  namespace body = kcds::runtime::body_state;
  const body::BodyPhysics physics = body::read_physics(local_actor);
  kcd2s::integration::PlayerPosture posture;
  posture.in_air = body::in_air(physics);
  kcds::runtime::knockout::note_in_air(posture.in_air);
  const bool seated = !kcds::runtime::seats::local_seats().empty();
  const body::BodyTags tags = body::read_tags(local_actor, seated);
  posture.crouched = tags.crouched;
  // The stance the body's animation chose, as Player.bodyStance reads it (sitting while seated).
  posture.stance = tags.stance;
  posture.physics_profile = physics.profile;
  // The combat core (actor + 632, its + 528): slot 1 says whether the body is in combat (as the
  // hit path asks), and the zone at + 384 (read through slot 1, as the attack factory
  // does) is the combat-star direction it aims at, a row of the game's zone table.
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) + 632);
  auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
  const bool in_combat = core && slot<char(__fastcall*)(void*)>(core, 1)(core);
  // The soul's state flags (reads them, soul + 2808 + 384): bit 3 while
  // an attack is held (HasRaisedWeapon), bit 4 IsBlocking.
  std::uint32_t soul_flags = 0;
  if (auto soul =
          *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) + 1640)) {
    soul_flags = *reinterpret_cast<const std::uint32_t*>(soul + 2808 + 384);
  }
  // Fists or weapon up: in combat, or holding an attack.
  posture.raised = in_combat || ((soul_flags >> 3) & 1) != 0;
  posture.guard = ((soul_flags >> 4) & 1) != 0;
  // IsOnLadder (actor vtable + 1288, as the script binding asks): other games keep
  // their player off the ladder meanwhile (movement/shared_ladders.hpp), and hold its
  // climb clip where the ladder action holds it here (hold_ladder_clip). Getting on or off, the
  // game plays the base layer on its own clock, and so do the replicas.
  posture.on_ladder = slot<bool(__fastcall*)(void*)>(local_actor, kActorIsOnLadder)(local_actor);
  // In the game's exhaustion faint (as tick_knockout saw it this frame): replicas lie
  // down by their own games instead of by a knock-out's pose.
  posture.fainted = kcds::runtime::knockout::fainted();
  if (posture.on_ladder) {
    int clip = -1;
    float progress = 0.0f;
    const bool held = local_held_climb(clip, progress);
    if (held) {
      posture.ladder_progress = kcd2s::integration::ladder_progress_of(progress);
    }
    static ULONGLONG next_climb_log = 0;
    if (GetTickCount64() >= next_climb_log) {
      next_climb_log = GetTickCount64() + 1000;
      log("LADDER local climb clip=%d held=%u progress=%.3f", clip, unsigned(held), progress);
    }
  }
  if (in_combat) {
    const unsigned zone = slot<unsigned(__fastcall*)(void*)>(core + 384, 1)(core + 384);
    if (zone < 255) {
      posture.combat_zone = std::int16_t(zone);
    }
  }
  // Logged when it changes, the climb's progress (every frame on a ladder) aside.
  static kcd2s::integration::PlayerPosture logged;
  kcd2s::integration::PlayerPosture shown = posture;
  shown.ladder_progress = 0;
  if (!(shown == logged)) {
    logged = shown;
    log("POSTURE in_air=%u crouched=%u guard=%u raised=%u ladder=%u fainted=%u zone=%d "
        "profile=%u (flying=%u for %.2f s)",
        unsigned(posture.in_air), unsigned(posture.crouched), unsigned(posture.guard),
        unsigned(posture.raised), unsigned(posture.on_ladder), unsigned(posture.fainted),
        int(posture.combat_zone), unsigned(posture.physics_profile), unsigned(physics.flying),
        physics.time_flying);
  }
  return kcd2s::integration::posture_field(posture);
}

}  // namespace kcds::game_client
