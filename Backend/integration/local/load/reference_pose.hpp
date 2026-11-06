#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../animation.hpp"
#include "../human_overlay.hpp"
#include "../movement.hpp"
#include "kcd2s/integration/player_stats.hpp"
#include "kcd2s/integration/animation_props.hpp"
#include "kcd2s/integration/appearance.hpp"
#include "kcd2s/integration/carry.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/grime.hpp"
#include "kcd2s/integration/hands.hpp"
#include "kcd2s/integration/posture.hpp"
#include "kcd2s/integration/player_input_publication.hpp"
#include "kcd2s/integration/seats.hpp"
#include "kcd2s/integration/station_action.hpp"
#include "kcd2s/replication/snapshot.hpp"
#include "load_options.hpp"

namespace kcd2s::local::load {

using ReferenceInput = std::vector<std::pair<core::FieldId, replication::FieldValue>>;

struct ReferencePose {
  core::Vec3 position;
  ReferenceInput fields;
  std::int64_t owner_clock_ms = 0;
  std::uint8_t epoch = 0;
};

inline bool reference_vector(const core::Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
         std::abs(value.x) <= 1000000 && std::abs(value.y) <= 1000000 &&
         std::abs(value.z) <= 1000000;
}

inline bool reference_rotation(const core::Quat& value) {
  const float length =
      value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
  return std::isfinite(length) && length > 0.8f && length < 1.2f;
}

// One instance belongs to one probe avatar. Capture before its first reference input. Later
// owner echoes never relocate it; only an explicit server force may replace the captured pose.
class ReferenceSpawnPosition {
 public:
  bool capture(const replication::Mirror::Entity* state) {
    if (position_) {
      return true;
    }
    if (!state || state->type != core::type_id("NetworkPlayer") ||
        state->fields.size() <= Position) {
      return false;
    }
    const auto* position = std::get_if<core::Vec3>(&state->fields[Position]);
    if (!position || !reference_vector(*position)) {
      return false;
    }
    position_ = *position;
    return true;
  }

  const std::optional<core::Vec3>& get() const {
    return position_;
  }

  bool force_position(const core::Vec3& position) {
    if (!reference_vector(position)) {
      return false;
    }
    position_ = position;
    return true;
  }

 private:
  std::optional<core::Vec3> position_;
};

// Translation fixture only: retain the server's initial height, then apply the
// real source's XYZ displacement. Ground height at translated XY is not validated.
class ReferenceFollowPosition {
 public:
  void source_missing() {
    if (last_position_) {
      anchor_ = last_position_;
    }
    origin_.reset();
    source_epoch_.reset();
    source_clock_.reset();
  }

  void force(const integration::ClientPeer::ForcedTransform& transform) {
    identity(transform.entity, transform.revision);
    source_missing();
    epoch_ = transform.epoch;
    if (transform.has_position) {
      anchor_ =
          reference_vector(transform.position) ? std::optional{transform.position} : std::nullopt;
      last_position_ = anchor_;
    }
  }

  std::optional<core::Vec3> observe(const ReferencePose& source, core::EntityId avatar,
                                    std::uint32_t revision, std::optional<std::uint8_t> epoch,
                                    const replication::Mirror::Entity* own,
                                    const std::optional<core::Vec3>& spawn) {
    identity(avatar, revision);
    if (!avatar || !epoch || !reference_vector(source.position) || source.owner_clock_ms <= 0) {
      source_missing();
      return std::nullopt;
    }
    if (!epoch_ || *epoch_ != *epoch || !anchor_) {
      // An epoch change without a drained position force must wait for a
      // matching authoritative snapshot, never borrow an old owner echo.
      if (!own || own->type != core::type_id("NetworkPlayer") || own->epoch != *epoch ||
          own->fields.size() <= Position) {
        return std::nullopt;
      }
      const auto* current = std::get_if<core::Vec3>(&own->fields[Position]);
      if (!current || !reference_vector(*current)) {
        return std::nullopt;
      }
      const auto at = !epoch_ && spawn ? spawn : std::optional{*current};
      if (!reference_vector(*at)) {
        return std::nullopt;
      }
      source_missing();
      anchor_ = at;
      last_position_ = at;
      epoch_ = epoch;
    }
    if (source_epoch_ && *source_epoch_ != source.epoch) {
      if (static_cast<std::int8_t>(source.epoch - *source_epoch_) <= 0) {
        return std::nullopt;
      }
      source_missing();
    }
    if (source_clock_ && source.owner_clock_ms < *source_clock_) {
      source_missing();
    }
    if (!origin_) {
      origin_ = source.position;
    }
    source_epoch_ = source.epoch;
    source_clock_ = source.owner_clock_ms;
    const core::Vec3 translated{anchor_->x + source.position.x - origin_->x,
                                anchor_->y + source.position.y - origin_->y,
                                anchor_->z + source.position.z - origin_->z};
    if (!reference_vector(translated)) {
      return std::nullopt;
    }
    last_position_ = translated;
    return translated;
  }

 private:
  void identity(core::EntityId avatar, std::uint32_t revision) {
    if (avatar_ != avatar || !revision_ || *revision_ != revision) {
      *this = {};
      avatar_ = avatar;
      revision_ = revision;
    }
  }

  core::EntityId avatar_ = 0;
  std::optional<std::uint32_t> revision_;
  std::optional<std::uint8_t> epoch_, source_epoch_;
  std::optional<std::int64_t> source_clock_;
  std::optional<core::Vec3> anchor_, origin_, last_position_;
};

// The headless reference actor must consume the same forced-state handoff as the native client.
// Merely receiving a FORCE_XFORM does not adopt its ownership epoch: without this drain, later
// pose inputs can be rejected forever while the transport still reports a connected player.
inline std::size_t adopt_reference_transforms(integration::ClientPeer& peer, core::EntityId avatar,
                                              ReferenceSpawnPosition& spawn,
                                              ReferenceFollowPosition* follow = nullptr) {
  if (avatar == core::kInvalidEntity) {
    return 0;
  }
  std::size_t adopted = 0;
  for (const auto& transform : peer.take_forced_transforms()) {
    if (transform.entity == avatar) {
      if (transform.has_position) {
        spawn.force_position(transform.position);
      }
      if (follow) {
        follow->force(transform);
      }
      // Its next authored orientation follows the real reference, just as its native motion and
      // appearance do. Position is fixed by the server only for the spawn-layout workload.
      ++adopted;
    }
  }
  return adopted;
}

// Preserve the reference's appearance and activity rather than allowing schema defaults
// to hide the bodies or turn a busy reference into an apparently idle crowd.
inline bool reference_text(core::FieldId field, const std::string& value) {
  if (value.empty()) {
    return true;
  }
  switch (field) {
    case Vitals: {
      integration::PlayerVitals decoded;
      return integration::parse_vitals_field(value, decoded);
    }
    case Abilities: {
      integration::PlayerAbilities decoded;
      return integration::parse_abilities_field(value, decoded);
    }
    case Outfit: {
      // The player's outfit codec (appearance/outfit.cpp): KO1 class IDs, or KO2
      // class IDs plus quality/health, with at most 40 worn pieces.
      const std::size_t piece = value.starts_with("KO1") ? 16 : 19;
      return (value.starts_with("KO1") || value.starts_with("KO2")) &&
             (value.size() - 3) % piece == 0 && (value.size() - 3) / piece <= 40;
    }
    case Appearance: {
      integration::Appearance decoded;
      return integration::parse_appearance_field(value, decoded);
    }
    case BodyParts: {
      integration::BodyParts decoded;
      return integration::parse_body_parts_field(value, decoded);
    }
    case Disguise:
      return integration::valid_disguise(value);
    case Hands: {
      integration::Hands decoded;
      return integration::parse_hands_field(value, decoded);
    }
    case WeaponAnimations:
      return decode_animation(value).has_value();
    case HumanOverlayAnimation:
      return decode_human_overlay(value).has_value();
    case RagdollPose:
      // The owner emits KSR1 plus at most 48 native 28-byte part transforms.
      return value.size() > 4 && value.starts_with("KSR1") && (value.size() - 4) % 28 == 0 &&
             (value.size() - 4) / 28 <= 48;
    case Seats: {
      std::set<integration::Seat> decoded;
      return integration::parse_seats_field(value, decoded);
    }
    case Carry: {
      integration::Guid decoded;
      return integration::parse_carry_field(value, decoded);
    }
    case StationAction: {
      integration::StationAction decoded;
      return integration::parse_station_action_field(value, decoded);
    }
    case Grime: {
      integration::Grime decoded;
      return integration::parse_grime_field(value, decoded);
    }
    case AnimationProps: {
      std::string style, animation, props;
      std::optional<integration::ShippedClip> clip;
      std::vector<integration::AnimationProp> decoded;
      return integration::split_talking_field(value, style, animation) &&
             integration::split_animation_field(animation, clip, props) &&
             integration::parse_animation_props_field(props, decoded);
    }
    default:
      return false;
  }
}

// A real player's replicated native data is the only source of animation clips and motion
// parameters. Missing, malformed, invalid or mounted references never produce synthetic input.
inline std::optional<ReferencePose> read_reference_pose(const replication::Mirror::Entity* state) {
  if (!state || state->type != core::type_id("NetworkPlayer") ||
      state->fields.size() <= VisorClosed) {
    return std::nullopt;
  }
  const auto* pose_valid = std::get_if<bool>(&state->fields[PoseValid]);
  const auto* motion_valid = std::get_if<bool>(&state->fields[MotionValid]);
  const auto* animations = std::get_if<std::string>(&state->fields[Animations]);
  const auto* rider = std::get_if<std::string>(&state->fields[NativeRiderState]);
  const auto* clock = std::get_if<std::int64_t>(&state->fields[MotionClock]);
  if (!pose_valid || !*pose_valid || !motion_valid || !*motion_valid || !animations ||
      !decode_animation(*animations) || !rider || !rider->empty() || !clock || *clock <= 0) {
    return std::nullopt;
  }
  ReferencePose result;
  result.owner_clock_ms = *clock;
  result.epoch = state->epoch;
  for (const auto field : {Position, Velocity, MotionVelocity, MotionParams0, MotionParams1,
                           MotionParams2, MotionParams3, MotionParams4, MotionParams5}) {
    const auto* value = std::get_if<core::Vec3>(&state->fields[field]);
    if (!value || !reference_vector(*value)) {
      return std::nullopt;
    }
    if (field == Position) {
      result.position = *value;
    } else {
      result.fields.emplace_back(field, *value);
    }
  }
  for (const auto field : {Rotation, MotionRotation}) {
    const auto* value = std::get_if<core::Quat>(&state->fields[field]);
    if (!value || !reference_rotation(*value)) {
      return std::nullopt;
    }
    result.fields.emplace_back(field, *value);
  }
  for (const auto field :
       {Gait, MotionType, MotionFlags, MotionMask, Posture, HeldProp, WeaponState}) {
    const auto* value = std::get_if<std::int64_t>(&state->fields[field]);
    if (!value) {
      return std::nullopt;
    }
    if ((field == MotionType && (*value < 0 || *value > 6)) ||
        (field == MotionFlags && (*value < 0 || *value > 3)) ||
        (field == MotionMask && (*value < 0 || *value > 0x1ffff)) ||
        (field == HeldProp && (*value < 0 || *value > 3)) ||
        (field == WeaponState && (*value < 0 || (*value & ~std::int64_t{7}) != 0))) {
      return std::nullopt;
    }
    if (field == Posture) {
      integration::PlayerPosture posture;
      if (!integration::parse_posture_field(*value, posture)) {
        return std::nullopt;
      }
    }
    result.fields.emplace_back(field, *value);
  }
  for (const auto field : {Grounded, PoseValid, MotionValid, BodyVisible, AimEnabled, UiBlocking,
                           VisorClosed, LookEnabled, LookValid}) {
    const auto* value = std::get_if<bool>(&state->fields[field]);
    if (!value) {
      return std::nullopt;
    }
    result.fields.emplace_back(field, *value);
  }
  for (const auto field : {LookTarget, LookTuning, LookDirection, AimTarget}) {
    const auto* value = std::get_if<core::Vec3>(&state->fields[field]);
    if (!value || !reference_vector(*value)) {
      return std::nullopt;
    }
    result.fields.emplace_back(field, *value);
  }
  for (const auto field :
       {Outfit, Appearance, BodyParts, Disguise, Hands, WeaponAnimations, HumanOverlayAnimation,
        RagdollPose, Seats, Carry, StationAction, Grime, AnimationProps, Vitals, Abilities}) {
    const auto* value = std::get_if<std::string>(&state->fields[field]);
    if (!value || !reference_text(field, *value)) {
      return std::nullopt;
    }
    result.fields.emplace_back(field, *value);
  }
  result.fields.emplace_back(Animations, *animations);
  result.fields.emplace_back(NativeRiderState, *rider);
  return result;
}

// Only an explicit path enables this one-time export. Preserve the received wire bytes exactly,
// including NULs and their codec version; a failed open/write/close must abort the measurement.
class ReferenceAnimationCapture {
 public:
  explicit ReferenceAnimationCapture(std::string path) : path_(std::move(path)) {
  }

  bool observe(const replication::Mirror::Entity* state) {
    if (path_.empty() || captured_ || !state || state->type != core::type_id("NetworkPlayer") ||
        state->fields.size() <= Animations) {
      return false;
    }
    const auto* bytes = std::get_if<std::string>(&state->fields[Animations]);
    if (!bytes || !decode_animation(*bytes)) {
      return false;
    }
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("reference-capture: cannot open " + path_);
    }
    output.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
    output.close();
    if (!output) {
      throw std::runtime_error("reference-capture: cannot write " + path_);
    }
    bytes_ = bytes->size();
    captured_ = true;
    return true;
  }

  bool completed() const {
    return captured_;
  }
  std::size_t bytes() const {
    return bytes_;
  }

 private:
  std::string path_;
  bool captured_ = false;
  std::size_t bytes_ = 0;
};

// One bounded diagnostic for the shared source, independent of the number of synthetic peers.
// Count distinct owner-clock observations and exact semantic changes; do not retain payload logs.
class ReferenceDiagnostics {
 public:
  bool observe(const replication::Mirror::Entity* state, std::uint32_t revision) {
    if (!revision_ || *revision_ != revision) {
      revision_ = revision;
      previous_.reset();
    }
    if (!state || state->fields.size() <= MotionClock) {
      return false;
    }
    const auto* clock = std::get_if<std::int64_t>(&state->fields[MotionClock]);
    if (!clock || (previous_ && previous_->owner_clock_ms == *clock)) {
      return false;
    }
    auto incoming = read_reference_pose(state);
    if (!incoming) {
      return false;
    }
    ++observations_;
    const bool idle = integration::PlayerInputPublication::idle(state->fields);
    idle_observations_ += idle;
    const bool first = !first_;
    if (first) {
      first_ = *state;
      first_idle_ = idle;
    }
    if (previous_) {
      static const auto schema = player_schema();
      const auto* type = schema.get(core::type_id("NetworkPlayer"));
      if (!replication::same_encoding(type->fields[Position], previous_->position,
                                      incoming->position)) {
        ++changes_[Position];
      }
      for (std::size_t i = 0; i < incoming->fields.size(); ++i) {
        const auto& [field, value] = incoming->fields[i];
        if (!integration::same_player_idle_field(type->fields[field], previous_->fields[i].second,
                                                 value)) {
          ++changes_[field];
        }
      }
    }
    previous_ = std::move(incoming);
    return first;
  }

  const replication::Mirror::Entity* first() const {
    return first_ ? &*first_ : nullptr;
  }
  bool first_idle() const {
    return first_idle_;
  }
  std::uint64_t observations() const {
    return observations_;
  }
  std::uint64_t idle_observations() const {
    return idle_observations_;
  }
  const auto& changes() const {
    return changes_;
  }

 private:
  std::optional<std::uint32_t> revision_;
  std::optional<ReferencePose> previous_;
  std::optional<replication::Mirror::Entity> first_;
  std::array<std::uint64_t, VisorClosed + 1> changes_{};
  std::uint64_t observations_ = 0;
  std::uint64_t idle_observations_ = 0;
  bool first_idle_ = false;
};

// One source per probe process. Frozen mode needs two strictly idle observations at least
// 500 owner-clock milliseconds apart, with 500 ms elapsed locally and no semantic change.
// Re-reading a single stale snapshot cannot confirm stability. Native playback still advances;
// this stores its input seed so later activity of the real observer cannot animate the crowd.
class ReferenceSource {
 public:
  explicit ReferenceSource(ReferenceUpdates updates) : updates_(updates) {
  }

  const ReferencePose* observe(const replication::Mirror::Entity* state, std::uint32_t revision,
                               double now) {
    if (!revision_ || *revision_ != revision) {
      revision_ = revision;
      candidate_.reset();
      selected_.reset();
    }
    if (updates_ == ReferenceUpdates::FrozenIdle && selected_) {
      return &*selected_;
    }
    auto incoming = read_reference_pose(state);
    if (updates_ == ReferenceUpdates::Live) {
      selected_ = std::move(incoming);
      return selected_ ? &*selected_ : nullptr;
    }
    if (!incoming || !std::isfinite(now) ||
        !integration::PlayerInputPublication::idle(state->fields)) {
      candidate_.reset();
      return nullptr;
    }
    if (!candidate_ || now < candidate_at_ ||
        incoming->owner_clock_ms < candidate_->owner_clock_ms || !same(*candidate_, *incoming)) {
      candidate_ = std::move(incoming);
      candidate_at_ = now;
      return nullptr;
    }
    if (now - candidate_at_ < 0.5 || incoming->owner_clock_ms - candidate_->owner_clock_ms < 500) {
      return nullptr;
    }
    selected_ = std::move(incoming);
    candidate_.reset();
    return &*selected_;
  }

  bool frozen() const {
    return updates_ == ReferenceUpdates::FrozenIdle && selected_.has_value();
  }

 private:
  static bool same(const ReferencePose& a, const ReferencePose& b) {
    if (a.position != b.position || a.fields.size() != b.fields.size()) {
      return false;
    }
    static const auto schema = player_schema();
    const auto* type = schema.get(core::type_id("NetworkPlayer"));
    for (std::size_t i = 0; i < a.fields.size(); ++i) {
      const auto& [field, value] = a.fields[i];
      if (field != b.fields[i].first ||
          !integration::same_player_idle_field(type->fields[field], value, b.fields[i].second)) {
        return false;
      }
    }
    return true;
  }

  ReferenceUpdates updates_;
  std::optional<std::uint32_t> revision_;
  std::optional<ReferencePose> candidate_;
  std::optional<ReferencePose> selected_;
  double candidate_at_ = 0;
};

// Where a spawn-walk player is on its circle at a moment of its own clock.
struct WalkPose {
  core::Vec3 position;
  core::Vec3 velocity;
  core::Quat rotation;
};

inline WalkPose walk_pose(const core::Vec3& spawn, std::size_t index, std::int64_t owner_clock_ms) {
  // KCDS_PROBE_FIGHT: the crowd fights on the spot, turning slowly.
  static const bool fight = std::getenv("KCDS_PROBE_FIGHT") != nullptr;
  const double kRadiusMeters = fight ? 0.2 : 2.5;
  const double kSpeedMetersPerSecond = fight ? 0.1 : 2.5;
  constexpr double kFullTurn = 6.283185307179586;
  // Spread over the crowd, so its players do not all turn the same way at once.
  const double phase = kFullTurn * std::fmod(double(index) * 0.618033988749895, 1.0);
  const double seconds = double(owner_clock_ms % 3600000) / 1000.0;
  const double angle = phase + seconds * kSpeedMetersPerSecond / kRadiusMeters;
  WalkPose pose;
  pose.position = {spawn.x + float(kRadiusMeters * std::cos(angle)),
                   spawn.y + float(kRadiusMeters * std::sin(angle)), spawn.z};
  pose.velocity = {float(-std::sin(angle) * kSpeedMetersPerSecond),
                   float(std::cos(angle) * kSpeedMetersPerSecond), 0.0f};
  // The game's forward is +y: a turn about z of the walk's direction from there.
  const double heading = std::atan2(double(-pose.velocity.x), double(pose.velocity.y));
  pose.rotation = {0.0f, 0.0f, float(std::sin(heading / 2.0)), float(std::cos(heading / 2.0))};
  return pose;
}

// A spawn-walk crowd's own walk: a few seconds of the clips the reference played while it walked,
// looped. Each round raises the clips' sequences, so a replica starts them again as it does for a
// real owner, and the crowd keeps walking whatever the real player does afterwards.
class WalkClipLoop {
 public:
  static constexpr std::int64_t kRoundMs = 4000;

  // Feeds the reference until a whole round is captured.
  void observe(const ReferencePose& reference) {
    if (complete()) {
      return;
    }
    const std::string* animations = nullptr;
    bool walking = false;
    for (const auto& [field, value] : reference.fields) {
      if (field == Animations) {
        animations = std::get_if<std::string>(&value);
      } else if (field == Velocity) {
        const auto* velocity = std::get_if<core::Vec3>(&value);
        walking = velocity && velocity->x * velocity->x + velocity->y * velocity->y > 1.0f;
      }
    }
    // KCDS_PROBE_FIGHT=<seconds>: that long after the first pose, the round is whatever the
    // reference plays, walking or not (its attacks).
    static const char* const fight_after = std::getenv("KCDS_PROBE_FIGHT");
    bool fight = false;
    if (fight_after) {
      if (first_seen_ms_ == 0) {
        first_seen_ms_ = reference.owner_clock_ms;
      }
      fight = reference.owner_clock_ms - first_seen_ms_ >= std::atoll(fight_after) * 1000;
    }
    if ((!walking && !fight) || !animations) {
      entries_.clear();
      return;
    }
    if (entries_.empty()) {
      started_ms_ = reference.owner_clock_ms;
    }
    if (entries_.empty() || entries_.back().bytes != *animations) {
      const auto state = decode_animation(*animations);
      if (state) {
        entries_.push_back({reference.owner_clock_ms - started_ms_, *state, *animations});
      }
    }
    if (reference.owner_clock_ms - started_ms_ >= kRoundMs && entries_.size() >= 2) {
      // At full speed by now: its motion values are the walk's, not its first step's.
      pose_ = reference;
      finish();
    }
  }

  bool complete() const {
    return span_ != 0;
  }

  // The reference's walking pose, once the round is captured.
  const ReferencePose& pose() const {
    return *pose_;
  }

  // The clips player `index` plays at its clock.
  const std::string& clips(std::size_t index, std::int64_t owner_clock_ms) {
    // Spread over the round, so a crowd does not step in time.
    const std::int64_t at = owner_clock_ms + std::int64_t(index) * 137;
    const std::int64_t round = at / kRoundMs;
    const std::int64_t into = at % kRoundMs;
    std::size_t entry = 0;
    while (entry + 1 < entries_.size() && entries_[entry + 1].at_ms <= into) {
      ++entry;
    }
    const std::uint64_t key = std::uint64_t(round) * entries_.size() + entry;
    const auto cached = encoded_.find(key);
    if (cached != encoded_.end()) {
      return cached->second;
    }
    AnimationState state = entries_[entry].state;
    for (unsigned layer = 0; layer < state.size(); ++layer) {
      if (moves_[layer]) {
        state[layer].sequence += std::uint64_t(round) * span_;
      }
    }
    if (encoded_.size() > kMostEncoded) {
      encoded_.clear();
    }
    return encoded_.emplace(key, encode_animation(state)).first->second;
  }

 private:
  static constexpr std::size_t kMostEncoded = 4096;

  struct Entry {
    std::int64_t at_ms;
    AnimationState state;
    std::string bytes;
  };

  void finish() {
    const AnimationState& first = entries_.front().state;
    const AnimationState& last = entries_.back().state;
    std::uint64_t span = 1;
    for (unsigned layer = 0; layer < first.size(); ++layer) {
      moves_[layer] = last[layer].sequence != first[layer].sequence;
      if (moves_[layer]) {
        span = std::max<std::uint64_t>(span, last[layer].sequence - first[layer].sequence + 1);
      }
    }
    span_ = span;
  }

  std::optional<ReferencePose> pose_;
  std::int64_t started_ms_ = 0;
  std::int64_t first_seen_ms_ = 0;
  std::vector<Entry> entries_;
  std::array<bool, 16> moves_{};
  std::uint64_t span_ = 0;
  std::unordered_map<std::uint64_t, std::string> encoded_;
};

// The ring has no synthetic orbit or native movement commands. Its centre follows the real
// player, and a stationary reference therefore leaves every replica stationary too.
inline ReferenceInput reference_input(const ReferencePose& reference, const LoadOptions& options,
                                      std::size_t index, std::int64_t owner_clock_ms,
                                      const std::optional<core::Vec3>& spawn = std::nullopt,
                                      const std::optional<core::Vec3>& follow = std::nullopt,
                                      const std::optional<core::Vec3>& keep_off = std::nullopt) {
  const bool walks = options.reference_layout == ReferenceLayout::SpawnWalk;
  if ((options.reference_layout == ReferenceLayout::Spawn || walks) &&
      (!spawn || !reference_vector(*spawn))) {
    return {};
  }
  if (options.reference_layout == ReferenceLayout::SpawnFollow &&
      (!follow || !reference_vector(*follow))) {
    return {};
  }
  ReferenceInput input = reference.fields;
  const double angle = 6.283185307179586 * double(index) / double(options.total_players());
  core::Vec3 position{reference.position.x + options.dense_radius_meters * float(std::cos(angle)),
                      reference.position.y + options.dense_radius_meters * float(std::sin(angle)),
                      reference.position.z};
  if (options.reference_layout == ReferenceLayout::Spawn) {
    position = *spawn;
  } else if (options.reference_layout == ReferenceLayout::SpawnFollow || walks) {
    const std::optional<WalkPose> walk =
        walks ? std::optional{walk_pose(*spawn, index, owner_clock_ms)} : std::nullopt;
    position = walks ? walk->position : *follow;
    // A synthetic player has no physics: without this it walks through the real player's camera,
    // where a real body would stop against the real player's.
    // The whole circle moves away, not the body on it: pushing bodies out at the edge of a keep-off
    // circle made them jump in every game that sees them.
    if (walks && keep_off) {
      constexpr float kKeepOffMeters = 2.0f + 2.5f;
      const float dx = spawn->x - keep_off->x;
      const float dy = spawn->y - keep_off->y;
      const float distance = std::sqrt(dx * dx + dy * dy);
      if (distance < kKeepOffMeters && distance > 0.001f) {
        const float push = kKeepOffMeters / distance - 1.0f;
        position.x += dx * push;
        position.y += dy * push;
      }
    }
    // Targets are world positions. Directions and tuning remain source values.
    for (auto& [field, value] : input) {
      if (walks && (field == Velocity || field == MotionVelocity)) {
        value = walk->velocity;
      } else if (walks && (field == Rotation || field == MotionRotation)) {
        value = walk->rotation;
      } else if (walks && field == RagdollPose) {
        // It walks whatever happens to the reference, and stays up.
        value = std::string{};
      } else if (walks && field == Vitals) {
        integration::PlayerVitals healthy;
        healthy.values.fill(100.0f);
        value = integration::vitals_field(healthy);
      } else if (field == LookTarget || field == AimTarget) {
        const auto* target = std::get_if<core::Vec3>(&value);
        if (!target) {
          return {};
        }
        const core::Vec3 translated{target->x + position.x - reference.position.x,
                                    target->y + position.y - reference.position.y,
                                    target->z + position.z - reference.position.z};
        if (!reference_vector(translated)) {
          return {};
        }
        value = translated;
      }
    }
  }
  input.emplace_back(Position, position);
  // MotionClock is the normal owner input clock (milliseconds), as in local_state_tick.cpp.
  // MotionMask retains the real native mask; it must never carry the transport-only marker here.
  input.emplace_back(MotionClock, owner_clock_ms);
  return input;
}

inline std::int64_t reference_marker_microseconds(std::int64_t owner_clock_ms) {
  if (owner_clock_ms <= 0 || owner_clock_ms > std::numeric_limits<std::int64_t>::max() / 1000) {
    return 0;
  }
  return owner_clock_ms * 1000;
}

}  // namespace kcd2s::local::load
