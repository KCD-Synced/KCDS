// Bound clock-only idle reports; every publication remains a complete owner input packet.
#pragma once

#include <array>
#include <cmath>
#include <vector>

#include "../../../local/movement.hpp"
#include "../../../local/animation.hpp"
#include "kcd2s/integration/posture.hpp"
#include "kcd2s/replication/field_codec.hpp"

namespace kcd2s::integration {

using PlayerInputFields = std::vector<std::pair<core::FieldId, replication::FieldValue>>;
inline constexpr double kPlayerIdlePublicationSeconds = 0.1;
inline constexpr double kPlayerIdleServerPublicationSeconds = 1.0;

// Use only for a fully validated idle player. Native FIFO capture advances normalized_time
// without a new start sequence; replicas already advance that same clip locally. Heartbeats
// still carry current progress. Starts, stops and every other parameter retain exact semantics.
inline bool same_player_idle_field(const replication::FieldDef& field,
                                   const replication::FieldValue& first,
                                   const replication::FieldValue& second) {
  if (replication::same_encoding(field, first, second)) {
    return true;
  }
  if (field.id != local::Animations && field.id != local::WeaponAnimations) {
    return false;
  }
  const auto* first_bytes = std::get_if<std::string>(&first);
  const auto* second_bytes = std::get_if<std::string>(&second);
  if (!first_bytes || !second_bytes) {
    return false;
  }
  const auto a = local::decode_animation(*first_bytes);
  const auto b = local::decode_animation(*second_bytes);
  if (!a || !b) {
    return false;
  }
  for (std::size_t i = 0; i < a->size(); ++i) {
    const auto& before = (*a)[i];
    const auto& after = (*b)[i];
    if (before.sequence != after.sequence || before.clip != after.clip) {
      return false;
    }
    auto before_params = before.params;
    auto after_params = after.params;
    if (before.sequence && before.clip >= 0) {
      before_params.normalized_time = after_params.normalized_time = 0;
    }
    if (std::memcmp(&before_params, &after_params, sizeof(local::AnimationParams)) != 0) {
      return false;
    }
  }
  return true;
}

struct PlayerInputScope {
  core::EntityId entity = 0;
  std::uint32_t owner = 0;
  std::uint8_t epoch = 0;
  std::uint32_t revision = 0;

  bool operator==(const PlayerInputScope& other) const {
    return entity == other.entity && owner == other.owner && epoch == other.epoch &&
           revision == other.revision;
  }
};

class PlayerInputPublication {
 public:
  // Missing activity evidence is never treated as idle. Do not use legacy Grounded/Gait:
  // the actual engine client reports Posture and native motion instead of those fields.
  static bool idle(const PlayerInputFields& fields) {
    std::array<const replication::FieldValue*, local::VisorClosed + 1> values{};
    for (const auto& [id, value] : fields) {
      if (id < values.size()) {
        if (values[id]) {
          return false;
        }
        values[id] = &value;
      }
    }
    return idle_fields([&](core::FieldId id) { return values[id]; });
  }

  static bool idle(const std::vector<replication::FieldValue>& fields) {
    return idle_fields([&](core::FieldId id) -> const replication::FieldValue* {
      return id < fields.size() ? &fields[id] : nullptr;
    });
  }

  // Both sender packets and replica states use this exact predicate without constructing or
  // copying field-pair vectors. Unknown, incomplete or active state never settles the timeline.
  template <class FieldAt>
  static bool idle_fields(FieldAt field_at) {
    auto boolean = [&](core::FieldId id, bool expected) {
      const auto* field = field_at(id);
      const auto* value = field ? std::get_if<bool>(field) : nullptr;
      return value && *value == expected;
    };
    auto empty = [&](core::FieldId id) {
      const auto* field = field_at(id);
      const auto* value = field ? std::get_if<std::string>(field) : nullptr;
      return value && value->empty();
    };
    auto integer = [&](core::FieldId id) {
      const auto* field = field_at(id);
      return field ? std::get_if<std::int64_t>(field) : nullptr;
    };
    auto stationary = [&](core::FieldId id) {
      const auto* field = field_at(id);
      const auto* value = field ? std::get_if<core::Vec3>(field) : nullptr;
      return value && value->x == 0 && value->y == 0 && value->z == 0;
    };
    const auto* posture_field = integer(local::Posture);
    const auto* clock = integer(local::MotionClock);
    const auto* flags = integer(local::MotionFlags);
    const auto* prop = integer(local::HeldProp);
    const auto* weapon = integer(local::WeaponState);
    PlayerPosture posture;
    return posture_field && parse_posture_field(*posture_field, posture) && !posture.in_air &&
           !posture.crouched && !posture.guard && !posture.raised && !posture.on_ladder &&
           !posture.fainted && posture.combat_zone < 0 && clock && *clock > 0 && flags &&
           (*flags & ~std::int64_t{1}) == 0 && prop && *prop == 0 && weapon && *weapon == 0 &&
           boolean(local::PoseValid, true) && boolean(local::MotionValid, true) &&
           boolean(local::AimEnabled, false) && stationary(local::Velocity) &&
           stationary(local::MotionVelocity) && empty(local::NativeRiderState) &&
           empty(local::RagdollPose) && empty(local::Seats) && empty(local::Carry) &&
           empty(local::StationAction) && empty(local::AnimationProps);
  }

  bool publish(const replication::TypeDef& type, const PlayerInputFields& fields,
               PlayerInputScope scope, double now, bool state_changed = false,
               bool honor_owner_refresh = false,
               double idle_refresh_seconds = kPlayerIdlePublicationSeconds) {
    if (type.id != core::type_id("NetworkPlayer") || !std::isfinite(now) ||
        !std::isfinite(idle_refresh_seconds) || idle_refresh_seconds <= 0 || !idle(fields)) {
      // Active input does not pay for copying its full appearance/animation strings into a cache.
      has_previous_ = false;
      previous_.clear();
      return true;
    }
    const double elapsed = now - published_at_;
    if (!state_changed && has_previous_ && scope == scope_ && elapsed >= 0 &&
        elapsed + 1e-8 < idle_refresh_seconds &&
        !(honor_owner_refresh && owner_refresh(fields, idle_refresh_seconds)) &&
        same_fields(type, fields)) {
      return false;
    }
    previous_ = fields;
    scope_ = scope;
    published_at_ = now;
    has_previous_ = true;
    return true;
  }

 private:
  bool owner_refresh(const PlayerInputFields& fields, double idle_refresh_seconds) const {
    auto clock = [](const PlayerInputFields& packet) -> std::int64_t {
      for (const auto& [id, value] : packet) {
        if (id == local::MotionClock) {
          const auto* number = std::get_if<std::int64_t>(&value);
          return number ? *number : 0;
        }
      }
      return 0;
    };
    const auto previous = clock(previous_);
    const auto current = clock(fields);
    // Independent gates otherwise turn tiny arrival jitter into a missed refresh.
    // This only allows a sender's refresh sooner; it neither rejects old clocks nor orders them.
    return previous > 0 && current > previous && current - previous >= idle_refresh_seconds * 1000;
  }

  bool same_fields(const replication::TypeDef& type, const PlayerInputFields& fields) const {
    if (fields.size() != previous_.size()) {
      return false;
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
      const auto& [id, value] = fields[i];
      if (id != previous_[i].first || id >= type.fields.size() ||
          (id != local::MotionClock &&
           !same_player_idle_field(type.fields[id], value, previous_[i].second))) {
        return false;
      }
    }
    return true;
  }

  PlayerInputFields previous_;
  PlayerInputScope scope_;
  double published_at_ = 0;
  bool has_previous_ = false;
};

}  // namespace kcd2s::integration
