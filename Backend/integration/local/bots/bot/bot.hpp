#pragma once
// One kcds-bots bot: a real KSTP client (local::Client, the same code the game client and the
// probes use, no second protocol) that joins as a bot (integration/bot_admission.hpp), plays its
// behaviour and measures what it sees. Every tick it sends its player's input as the game's
// frame does (the peer publishes what changed), drains the server's corrections, answers the
// blows that reach it, and turns its behaviour's intent into the messages a game client sends:
// combat inputs and contacts, shots, chat, voice, a horse and its riding.
//
// What a bot does not do that a game does: it runs no resource client scripts and downloads no
// resource files (it says its client resources are ready as soon as it is in the world), it
// emulates the engine's world-loaded callback, and its native combat records carry neutral values
// (no game state behind them) that pass the server's checks.
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../animation.hpp"
#include "../../rider_state.hpp"
#include "../../runtime.hpp"
#include "../behaviours/catalog.hpp"
#include "bot_stats.hpp"
#include "kcd2s/integration/bot_admission.hpp"
#include "kcd2s/integration/combat.hpp"
#include "kcd2s/integration/horse.hpp"
#include "kcd2s/integration/melee_reach.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "kcd2s/integration/shots.hpp"
#include "kcd2s/voice/frame.hpp"
#include "voice_loop.hpp"

namespace kcd2s::local::bots {

// How long a join may take before it counts as failed.
inline constexpr double kJoinTimeoutSeconds = 60.0;
// A bot's behaviour decides ten times a second; its input goes out every tick.
inline constexpr double kDecideEverySeconds = 0.1;
// A swing reaches its target this long after it starts (a contact then goes out when in reach).
inline constexpr double kSwingContactSeconds = 0.35;
// The arrow's speed, metres a second.
inline constexpr float kArrowSpeed = 60.0f;

// The nickname of a bot: bot-<group>-<index>.<run tag>. The run tag tells the bots of this
// process (one clock, so their inputs' ages can be measured) from those of another.
inline std::string bot_nickname(const std::string& group, std::size_t index,
                                const std::string& run_tag) {
  return "bot-" + group + "-" + std::to_string(index) + "." + run_tag;
}

struct NicknameParts {
  bool bot = false;
  std::string group;
  std::string run_tag;
};

inline NicknameParts parse_bot_nickname(const std::string& nickname) {
  NicknameParts parts;
  if (nickname.rfind("bot-", 0) != 0) {
    return parts;
  }
  const std::size_t dot = nickname.rfind('.');
  const std::size_t dash = nickname.rfind('-', dot == std::string::npos ? std::string::npos : dot);
  if (dot == std::string::npos || dash == std::string::npos || dash < 4) {
    return parts;
  }
  parts.bot = true;
  parts.group = nickname.substr(4, dash - 4);
  parts.run_tag = nickname.substr(dot + 1);
  return parts;
}

// What every bot of a run shares (read only while it runs).
struct RunShared {
  std::string host = "127.0.0.1";
  unsigned short port = 7777;
  // The address the bots' sockets bind: 127.0.0.1 for a server on this machine (no firewall
  // prompt), 0.0.0.0 for one across the network.
  std::string bind = "0.0.0.0";
  std::string run_tag;
  const VoiceLoop* voice = nullptr;
  std::array<std::uint8_t, 16> ammo{};
  std::uint64_t seed = 1;
};

// One bot's fixed plan.
struct BotPlan {
  std::size_t index = 0;  // in the whole run (all machines)
  const Group* group = nullptr;
  const Area* area = nullptr;  // nullptr: where the server spawned it
  double spawn_radius = 0.0;
  NetworkConditions network;
  double join_at = 0.0;  // seconds after the run started
};

class Bot {
 public:
  Bot(BotPlan plan, const RunShared& shared)
      : plan_(std::move(plan)),
        shared_(shared),
        random_(shared.seed, plan_.index),
        nickname_(bot_nickname(plan_.group->name, plan_.index, shared.run_tag)) {
    std::vector<std::string> problems;
    behaviour_ = make_behaviour(*plan_.group, problems);
    // This bot's own one-way delay within the scenario's range.
    delay_ms_ = random_.between(plan_.network.latency_ms);
  }

  // One tick: `now` seconds since the run started, `clock` the steady clock (clock_seconds()).
  void tick(double now, double clock, double dt) {
    switch (phase_) {
      case Phase::Waiting:
        if (now >= plan_.join_at) {
          start_session(now);
        }
        return;
      case Phase::Away:
        if (now >= rejoin_at_) {
          start_session(now);
        }
        return;
      case Phase::Failed:
      case Phase::Done:
        return;
      case Phase::Joining:
      case Phase::Playing:
        break;
    }
    client_->tick(clock);
    if (client_->refused()) {
      refused();
      return;
    }
    auto& peer = client_->peer();
    if (peer.world_config() && !peer.world_ready()) {
      // Headless: the engine's world-loaded callback, and its loading screen going down.
      peer.mark_world_ready(peer.world_config()->revision);
    }
    if (peer.world_ready() && !peer.gameplay_started()) {
      peer.mark_gameplay_started(peer.world_config()->revision);
    }
    if (phase_ == Phase::Joining) {
      join(now, clock);
      return;
    }
    play(now, clock, dt);
  }

  // The end of the run: leaves the server as a game does.
  void finish() {
    if (client_) {
      end_session();
    }
    phase_ = Phase::Done;
  }

  bool playing() const {
    return phase_ == Phase::Playing;
  }
  bool failed() const {
    return phase_ == Phase::Failed;
  }
  const BotStats& stats() const {
    return stats_;
  }
  // Bytes and datagrams of every session so far.
  metrics::TrafficCounters traffic() const {
    metrics::TrafficCounters total = finished_traffic_;
    if (client_) {
      add(total, client_->traffic());
    }
    return total;
  }
  const std::string& nickname() const {
    return nickname_;
  }
  const std::string& group() const {
    return plan_.group->name;
  }

 private:
  enum class Phase { Waiting, Joining, Playing, Away, Failed, Done };

  struct Seen {
    NicknameParts nickname;
    std::int64_t last_clock = 0;
    double last_update = -1.0;
  };

  static void add(metrics::TrafficCounters& total, const metrics::TrafficCounters& part) {
    total.packets_sent += part.packets_sent;
    total.bytes_sent += part.bytes_sent;
    total.packets_received += part.packets_received;
    total.bytes_received += part.bytes_received;
    total.send_would_block += part.send_would_block;
    total.send_dropped += part.send_dropped;
  }

  void start_session(double now) {
    client_ = std::make_unique<Client>(shared_.host.c_str(), shared_.port, shared_.bind.c_str());
    client_->set_identity(nickname_, "");
    client_->set_licence(std::string(integration::kBotLicence));
    netsim::ImpairmentSettings impairment;
    impairment.loss_fraction = plan_.network.loss;
    impairment.delay_seconds = delay_ms_ / 1000.0;
    impairment.jitter_seconds = plan_.network.jitter_ms / 1000.0;
    impairment.seed = random_.next() | 1u;
    client_->set_impairment(impairment);
    stats_.join_attempts += 1;
    join_started_ = now;
    phase_ = Phase::Joining;
    avatar_ = core::kInvalidEntity;
    seen_.clear();
    behaviour_ = rebuilt_behaviour();
    voice_session_ = 0;
    horse_ = core::kInvalidEntity;
    horse_asked_at_ = -100.0;
    mount_asked_at_ = -100.0;
    lock_ = 0;
    pending_contact_.reset();
  }

  std::unique_ptr<Behaviour> rebuilt_behaviour() {
    std::vector<std::string> problems;
    return make_behaviour(*plan_.group, problems);
  }

  void end_session() {
    client_->disconnect();
    add(finished_traffic_, client_->traffic());
    client_.reset();
  }

  void refused() {
    stats_.refused_joins += 1;
    std::string reason = client_->last_refusal_text();
    if (reason.empty()) {
      const auto code = client_->last_refusal();
      reason = code ? "refusal " + std::to_string(int(*code)) : "refused";
    }
    stats_.refusal_reasons[reason] += 1;
    add(finished_traffic_, client_->traffic());
    client_.reset();
    phase_ = Phase::Failed;
  }

  void join(double now, double clock) {
    auto& peer = client_->peer();
    if (peer.world_ready()) {
      avatar_ = client_->own_avatar();
    }
    if (avatar_ == core::kInvalidEntity) {
      if (now - join_started_ > kJoinTimeoutSeconds) {
        stats_.timed_out_joins += 1;
        end_session();
        phase_ = Phase::Failed;
      }
      return;
    }
    const auto* own = peer.mirror().get(avatar_);
    if (!own || own->fields.size() <= MotionClock) {
      return;  // its first snapshot is not there yet
    }
    const auto* position = std::get_if<core::Vec3>(&own->fields[Position]);
    position_ = position ? *position : core::Vec3{};
    spawn_ = position_;
    home_ = plan_.area ? area_centre(*plan_.area) : spawn_;
    home_radius_ = plan_.area ? (plan_.spawn_radius > 0.0 ? plan_.spawn_radius : plan_.area->radius)
                              : std::max(plan_.spawn_radius, 10.0);
    stats_.spawned += 1;
    stats_.connect_to_spawn_ms.add((now - join_started_) * 1000.0);
    // As the game does once in the world: its name for the others, and its client resources
    // running (a bot runs none, so they are ready at once; the server holds its events until then).
    peer.send_nickname(nickname_);
    peer.send_script(sdk::Value(sdk::Value::Map{{"op", sdk::Value("resourcesReady")}}));
    phase_ = Phase::Playing;
    next_decision_ = now;
    (void)clock;
  }

  core::Vec3 area_centre(const Area& area) const {
    if (area.absolute) {
      return {float(area.x), float(area.y), float(area.z)};
    }
    return {float(spawn_.x + area.x), float(spawn_.y + area.y), spawn_.z};
  }

  void play(double now, double clock, double dt) {
    last_now_ = now;
    stats_.playing_seconds += dt;
    auto& peer = client_->peer();
    if (!peer.owns(avatar_)) {
      // The server took the avatar (a new world revision): join it again.
      phase_ = Phase::Joining;
      avatar_ = core::kInvalidEntity;
      join_started_ = now;
      return;
    }
    apply_server_moves();
    acknowledge_scripts();
    observe(now, clock);
    answer_blows();
    take_voice_state();
    if (now >= next_decision_) {
      next_decision_ = now + kDecideEverySeconds;
      decide(now);
    }
    if (intent_.leave) {
      leave(now);
      return;
    }
    move(dt);
    act(now);
    send_input(clock);
    talk(now);
    if (mounted()) {
      ride();
    }
  }

  // The server's teleports and corrections, applied before the next input (the epoch they bring
  // must never carry a pose from before them).
  void apply_server_moves() {
    for (const auto& moved : client_->peer().take_forced_transforms()) {
      if (moved.entity != avatar_ || !moved.has_position) {
        continue;
      }
      position_ = moved.position;
      if (moved.correction) {
        stats_.corrections += 1;
      } else {
        stats_.teleports += 1;
      }
    }
  }

  // SDK entity frames are acknowledged as the game's resource runtime does.
  void acknowledge_scripts() {
    auto& peer = client_->peer();
    for (const sdk::Value& message : peer.take_scripts()) {
      const std::string op = integration::script_operation(message);
      if (op != "entitySession" && op != "entityBegin" && op != "entityChunk" &&
          op != "entityCommit") {
        continue;
      }
      const auto* session = integration::script_field(message, "session");
      const auto* delivery = integration::script_field(message, "delivery");
      if (session && delivery && session->tag() == sdk::Tag::Str) {
        sdk::Value::Map ack{
            {"op", sdk::Value("entityAck")}, {"session", *session}, {"delivery", *delivery}};
        peer.send_script(sdk::Value(std::move(ack)));
      }
    }
  }

  // After each new snapshot: the other players, how old the inputs of this process's bots are when
  // they arrive here (snapshot age), and how often each is updated by distance.
  void observe(double now, double clock) {
    auto& peer = client_->peer();
    const core::Tick baseline = peer.baseline();
    if (baseline == last_baseline_) {
      return;
    }
    last_baseline_ = baseline;
    stats_.snapshots += 1;
    neighbours_.clear();
    horse_ = core::kInvalidEntity;
    horse_rider_ = 0;
    horse_simulator_ = 0;
    horse_epoch_ = 0;
    static const core::TypeId kPlayer = core::type_id("NetworkPlayer");
    static const core::TypeId kHorse = core::type_id("NetworkHorse");
    const auto clock_ms = std::int64_t(std::llround(clock * 1000.0));
    for (const auto& [entity, state] : peer.mirror().entities()) {
      if (state.type == kHorse) {
        note_horse(entity, state);
        continue;
      }
      if (state.type != kPlayer || entity == avatar_ || state.fields.size() <= MotionClock) {
        continue;
      }
      auto& seen = seen_[entity];
      if (seen.last_update < 0.0) {
        const auto* nickname = std::get_if<std::string>(&state.fields[Nickname]);
        seen.nickname = parse_bot_nickname(nickname ? *nickname : std::string());
      }
      const auto* position = std::get_if<core::Vec3>(&state.fields[Position]);
      if (!position) {
        continue;
      }
      neighbours_.push_back({entity, seen.nickname.group, *position});
      const auto* motion_clock = std::get_if<std::int64_t>(&state.fields[MotionClock]);
      if (!motion_clock || *motion_clock <= seen.last_clock) {
        continue;
      }
      const bool known = seen.last_clock != 0;
      seen.last_clock = *motion_clock;
      if (known && seen.last_update >= 0.0) {
        const double metres = ground_distance(position_, *position);
        stats_.update_interval_ms[std::size_t(band_of(metres))].add((now - seen.last_update) *
                                                                    1000.0);
      }
      seen.last_update = now;
      if (known && seen.nickname.bot && seen.nickname.run_tag == shared_.run_tag) {
        stats_.snapshot_age_ms.add(double(clock_ms - *motion_clock));
      }
    }
  }

  void note_horse(core::EntityId entity, const replication::Mirror::Entity& state) {
    if (state.fields.size() <= integration::HorseEpoch) {
      return;
    }
    const auto* owner = std::get_if<std::int64_t>(&state.fields[integration::HorseOwner]);
    if (!owner || core::EntityId(*owner) != avatar_) {
      return;
    }
    horse_ = entity;
    // A horse's first field is its position (movement.hpp, NetworkHorse).
    if (const auto* where = std::get_if<core::Vec3>(&state.fields[0])) {
      horse_position_ = *where;
    }
    horse_rider_ = std::get<std::int64_t>(state.fields[integration::HorseRider]);
    horse_simulator_ = std::get<std::int64_t>(state.fields[integration::HorseSimulator]);
    horse_epoch_ = std::get<std::int64_t>(state.fields[integration::HorseEpoch]);
    if (!had_horse_) {
      had_horse_ = true;
      stats_.horses += 1;
    }
  }

  bool mounted() const {
    return horse_ != core::kInvalidEntity && core::EntityId(horse_rider_) == avatar_;
  }

  // A blow that reached this bot: its game's guard blocks part of them (a reaction), the rest
  // hurt it (the effects its game computed).
  void answer_blows() {
    auto& peer = client_->peer();
    for (const integration::MeleeEvent& event : peer.take_melee()) {
      if (!event.contact || event.target != avatar_) {
        continue;
      }
      stats_.contacts_received += 1;
      if (random_.chance(behaviour_->block_chance())) {
        integration::NativeReactionData reaction{};
        if (peer.send_reaction(event.source, avatar_, reaction, true)) {
          stats_.blocks += 1;
        }
        continue;
      }
      integration::NativeEffects effects(2);
      effects[0].kind = 1;  // HealthValue of the victim
      effects[0].soul_role = 2;
      const float health = -4.0f;
      std::memcpy(effects[0].value.data() + 4, &health, 4);
      effects[1].kind = 2;  // ActStaminaValue of the victim
      effects[1].soul_role = 2;
      const std::uint32_t stamina_attribute = 1;
      const float stamina = -8.0f;
      std::memcpy(effects[1].value.data(), &stamina_attribute, 4);
      std::memcpy(effects[1].value.data() + 4, &stamina, 4);
      integration::NativeMeleeData cause{};
      if (peer.send_native_effects(event.source, avatar_, cause, effects)) {
        stats_.effects_sent += 1;
      }
    }
  }

  // The voice session the server gives: this bot's keys for it.
  void take_voice_state() {
    auto& peer = client_->peer();
    const auto state = peer.take_voice_state();
    if (!state || !state->enabled || state->session == 0 || state->session == voice_session_) {
      return;
    }
    const auto keys = voice::make_key_pair();
    voice::Key server{};
    std::copy(state->server_key.begin(), state->server_key.end(), server.begin());
    voice::Key session_key{};
    if (!voice::derive_session_key(keys.secret_key, server, keys.public_key, server, session_key)) {
      return;
    }
    std::array<std::uint8_t, 32> own_public{};
    std::copy(keys.public_key.begin(), keys.public_key.end(), own_public.begin());
    client_->set_voice_endpoint(state->voice_ip, state->voice_port);
    if (peer.send_voice_key(own_public)) {
      voice_session_ = state->session;
      voice_key_ = session_key;
    }
  }

  void decide(double now) {
    Situation situation;
    situation.now = now;
    situation.position = position_;
    situation.home = home_;
    situation.home_radius = home_radius_;
    situation.others = &neighbours_;
    situation.mounted = mounted();
    situation.has_horse = horse_ != core::kInvalidEntity;
    situation.horse = horse_position_;
    intent_ = behaviour_->step(situation, random_);
  }

  void leave(double now) {
    stats_.leaves += 1;
    rejoin_at_ = intent_.rejoin_at;
    intent_ = Intent{};
    end_session();
    phase_ = Phase::Away;
    (void)now;
  }

  // Towards the intent's point at its speed; facing where it walks or what it looks at.
  void move(double dt) {
    const double gap = ground_distance(position_, intent_.move_to);
    const double step = std::min(gap, intent_.speed * dt);
    velocity_ = {};
    if (gap > 0.01 && step > 0.0) {
      const double dx = (intent_.move_to.x - position_.x) / gap;
      const double dy = (intent_.move_to.y - position_.y) / gap;
      position_.x += float(dx * step);
      position_.y += float(dy * step);
      velocity_ = {float(dx * intent_.speed), float(dy * intent_.speed), 0.0f};
      yaw_ = std::atan2(dy, dx);
    }
    if (intent_.face_point && ground_distance(position_, intent_.face) > 0.1) {
      yaw_ = std::atan2(double(intent_.face.y) - position_.y, double(intent_.face.x) - position_.x);
    }
  }

  void act(double now) {
    auto& peer = client_->peer();
    if (intent_.chat) {
      if (peer.send_chat(avatar_, *intent_.chat)) {
        stats_.chat_lines += 1;
      }
      intent_.chat.reset();
    }
    if (intent_.emote) {
      emote_until_ = now + 3.0;
      stats_.emotes += 1;
      intent_.emote = false;
    }
    act_melee(now);
    act_ranged(now);
    act_horse(now);
  }

  void act_melee(double now) {
    auto& peer = client_->peer();
    using integration::CombatAction;
    if (intent_.lock != lock_) {
      if (intent_.lock != 0) {
        peer.send_combat_action(avatar_, intent_.lock, {CombatAction::Target, 1, 1.0f});
      }
      lock_ = intent_.lock;
    }
    if (intent_.attack && lock_ != 0) {
      intent_.attack = false;
      peer.send_combat_action_to_viewers(avatar_, lock_, {CombatAction::Attack, 1, 0.0f});
      attack_generation_ += 1;
      peer.send_attack_generation(avatar_, lock_, attack_generation_, 3);
      stats_.swings += 1;
      pending_contact_ = PendingContact{now + kSwingContactSeconds, lock_, attack_generation_};
    }
    if (intent_.clinch && lock_ != 0) {
      intent_.clinch = false;
      peer.send_combat_action(avatar_, lock_, {CombatAction::Clinch, 1, 1.0f});
    }
    if (pending_contact_ && now >= pending_contact_->at) {
      const PendingContact contact = *pending_contact_;
      pending_contact_.reset();
      const auto* target = peer.mirror().get(contact.target);
      const auto* where = target && target->fields.size() > Position
                              ? std::get_if<core::Vec3>(&target->fields[Position])
                              : nullptr;
      // Only a swing that reaches lands, well within the server's reach check.
      if (where && ground_distance(position_, *where) < integration::kMeleeReach - 1.0f) {
        integration::NativeContactData data{};
        if (peer.send_native_contact(avatar_, contact.target, data, contact.generation)) {
          stats_.contacts_sent += 1;
        }
      }
    }
  }

  void act_ranged(double now) {
    if (!intent_.shoot) {
      return;
    }
    intent_.shoot = false;
    integration::Shot shot;
    shot.ammo = shared_.ammo;
    shot.position[0] = position_.x;
    shot.position[1] = position_.y;
    shot.position[2] = position_.z + 1.5f;
    const double dx = double(intent_.face.x) - position_.x;
    const double dy = double(intent_.face.y) - position_.y;
    const double dz = double(intent_.face.z + 1.2f) - (position_.z + 1.5f);
    const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length < 0.5) {
      return;
    }
    shot.direction[0] = float(dx / length);
    shot.direction[1] = float(dy / length);
    shot.direction[2] = float(dz / length);
    shot.speed = kArrowSpeed;
    if (client_->peer().send_shot(shot)) {
      stats_.shots += 1;
    }
    (void)now;
  }

  void act_horse(double now) {
    auto& peer = client_->peer();
    if (intent_.want_horse && horse_ == core::kInvalidEntity && now - horse_asked_at_ > 10.0) {
      // The bots' support resource gives a horse to a bot that asks (resource/kcds-bots).
      horse_asked_at_ = now;
      peer.send_script(sdk::Value(sdk::Value::Map{{"op", sdk::Value("event")},
                                                  {"name", sdk::Value("kcds-bots:horse")},
                                                  {"payload", sdk::Value("{}")}}));
    }
    if (intent_.mount && horse_ != core::kInvalidEntity && !mounted() &&
        now - mount_asked_at_ > 3.0) {
      mount_asked_at_ = now;
      if (peer.send_horse_mount(avatar_, horse_, true)) {
        stats_.mounts += 1;
      }
    }
  }

  // The rider simulates its horse, as the rider's game does.
  void ride() {
    if (core::EntityId(horse_simulator_) != avatar_) {
      return;
    }
    integration::HorseState state;
    state.source = avatar_;
    state.horse = horse_;
    state.sequence = ++horse_sequence_;
    state.position = position_;
    state.velocity = velocity_;
    state.rotation = yaw_quaternion();
    state.motion_rotation = state.rotation;
    state.motion_valid = true;
    state.animations = encode_animation(AnimationState{});
    client_->peer().send_horse_state(state);
  }

  core::Quat yaw_quaternion() const {
    // The game's yaw turns about z; its forward is +y, so a heading along +x is a quarter turn.
    const double heading = yaw_ - 3.14159265358979323846 / 2.0;
    return {0.0f, 0.0f, float(std::sin(heading / 2.0)), float(std::cos(heading / 2.0))};
  }

  std::string animations(double now) {
    std::int32_t clip = 100;  // standing
    if (now < emote_until_) {
      clip = 201;
    } else if (intent_.speed > kWalkSpeed + 0.1) {
      clip = 102;
    } else if (intent_.speed > 0.0) {
      clip = 101;
    }
    if (clip != animation_clip_) {
      animation_clip_ = clip;
      animation_sequence_ += 1;
    }
    AnimationState state{};
    state[0].sequence = animation_sequence_;
    state[0].clip = clip;
    state[0].params.flags = 0x102;
    return encode_animation(state);
  }

  void send_input(double clock) {
    const core::Quat rotation = yaw_quaternion();
    const bool aiming = intent_.aim;
    std::string rider;
    if (mounted()) {
      RiderState state;
      state.horse = horse_;
      state.generation = 1;
      state.epoch = std::uint32_t(horse_epoch_);
      state.phase = 1;
      rider = encode_rider(state);
    }
    std::vector<std::pair<core::FieldId, replication::FieldValue>> input{
        {Position, position_},
        {Rotation, rotation},
        {Velocity, velocity_},
        {PoseValid, true},
        {MotionValid, true},
        {MotionType, std::int64_t{1}},
        {MotionVelocity, velocity_},
        {MotionRotation, rotation},
        {MotionFlags, std::int64_t{1}},
        {LookEnabled, false},
        {LookValid, false},
        {NativeRiderState, rider},
        {Animations, animations(last_now_)},
        {Posture, std::int64_t{0}},
        {AimEnabled, aiming},
        {AimTarget, aiming ? intent_.face : position_},
        {WeaponState, std::int64_t{aiming ? 2 : 0}},
        {MotionClock, std::int64_t(std::llround(clock * 1000.0))}};
    for (unsigned i = 0; i < 6; ++i) {
      input.emplace_back(core::FieldId(MotionParams0 + i), core::Vec3{});
    }
    client_->peer().send_input(avatar_, input);
    stats_.inputs += 1;
  }

  // While talking: a 20 ms Opus frame each 20 ms, sealed with this bot's session key.
  void talk(double now) {
    if (!intent_.talking || voice_session_ == 0 || !shared_.voice) {
      next_voice_ = now;
      return;
    }
    std::vector<std::uint8_t> datagram;
    while (next_voice_ <= now) {
      next_voice_ += kVoiceFrameSeconds;
      const auto& frame = shared_.voice->frame(voice_counter_);
      voice::VoiceHeader header;
      header.tag = voice::kVoiceUpTag;
      header.id = voice_session_;
      header.counter = ++voice_counter_;
      header.loudness = 96;
      if (voice::seal_voice(header, voice_key_, frame.data(), frame.size(), datagram) &&
          client_->send_voice_datagram(datagram.data(), datagram.size())) {
        stats_.voice_frames += 1;
      }
    }
  }

  struct PendingContact {
    double at = 0.0;
    core::EntityId target = 0;
    std::uint64_t generation = 0;
  };

  BotPlan plan_;
  const RunShared& shared_;
  BotRandom random_;
  std::string nickname_;
  std::unique_ptr<Behaviour> behaviour_;
  double delay_ms_ = 0.0;

  Phase phase_ = Phase::Waiting;
  std::unique_ptr<Client> client_;
  metrics::TrafficCounters finished_traffic_;
  BotStats stats_;
  double join_started_ = 0.0;
  double rejoin_at_ = 0.0;

  core::EntityId avatar_ = core::kInvalidEntity;
  core::Vec3 position_;
  core::Vec3 velocity_;
  core::Vec3 spawn_;
  core::Vec3 home_;
  double home_radius_ = 10.0;
  double yaw_ = 0.0;
  double next_decision_ = 0.0;
  double last_now_ = 0.0;
  Intent intent_;

  core::Tick last_baseline_ = 0;
  std::unordered_map<core::EntityId, Seen> seen_;
  std::vector<Neighbour> neighbours_;

  core::EntityId lock_ = 0;
  std::uint64_t attack_generation_ = 0;
  std::optional<PendingContact> pending_contact_;

  double emote_until_ = -1.0;
  std::int32_t animation_clip_ = -1;
  std::uint64_t animation_sequence_ = 0;

  std::uint32_t voice_session_ = 0;
  voice::Key voice_key_{};
  std::uint64_t voice_counter_ = 0;
  double next_voice_ = 0.0;

  core::EntityId horse_ = core::kInvalidEntity;
  core::Vec3 horse_position_;
  std::int64_t horse_rider_ = 0;
  std::int64_t horse_simulator_ = 0;
  std::int64_t horse_epoch_ = 0;
  bool had_horse_ = false;
  std::uint64_t horse_sequence_ = 0;
  double horse_asked_at_ = -100.0;
  double mount_asked_at_ = -100.0;
};

}  // namespace kcd2s::local::bots
