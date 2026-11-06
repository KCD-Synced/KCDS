#pragma once

#include "kcd2s/integration/player_input_publication.hpp"
#include "kcd2s/integration/replica_timeline.hpp"

namespace kcd2s::integration {

// Presentation of a NetworkPlayer: delayed owner samples stay atomic with their pose, while
// server-authoritative fields and untimed corrections must not wait for another owner clock.
// State has the Mirror::Entity shape; the adapter is portable and does no native reads.
template <class State>
class PlayerReplicaTimeline {
 public:
  explicit PlayerReplicaTimeline(float teleport_meters) : timeline_(teleport_meters) {
  }

  const State& shown(const State& latest, double now, State& output) {
    static const auto schema = local::player_schema();
    const auto& type = *schema.get("NetworkPlayer");
    const auto* clock = clock_of(latest);
    const auto* position = field<core::Vec3>(latest, local::Position);
    const auto* rotation = field<core::Quat>(latest, local::Rotation);
    if (latest.type != type.id || !clock || *clock <= 0 || !position || !rotation ||
        !std::isfinite(now) || !std::isfinite(position->x) || !std::isfinite(position->y) ||
        !std::isfinite(position->z) || !std::isfinite(rotation->x) || !std::isfinite(rotation->y) ||
        !std::isfinite(rotation->z) || !std::isfinite(rotation->w)) {
      clear();
      return latest;
    }
    if (!has_epoch_ || latest.epoch != epoch_) {
      clear();
      epoch_ = latest.epoch;
      has_epoch_ = true;
    }
    const TimelinePose arrived{*position, *rotation};
    // A body nothing new came for has nothing new to add: most of a crowd on most frames.
    // 0: no mirror numbered this state, so it cannot be known unchanged.
    if (!has_revision_ || latest.revision == 0 || latest.revision != revision_) {
      has_revision_ = true;
      revision_ = latest.revision;
      const bool stationary = PlayerInputPublication::idle(latest.fields);
      if (!timeline_.add(*clock, latest, arrived, now, stationary)) {
        const auto* newest = timeline_.newest();
        if (newest && !same_state(type, *newest, latest)) {
          timeline_.refresh_newest(*clock, latest, arrived, stationary);
        }
      }
    }
    TimelinePose pose;
    const auto* selected = timeline_.shown(now, pose);
    if (!selected) {
      return latest;
    }
    const auto* selected_clock = clock_of(*selected);
    if (selected_clock && *selected_clock == *clock) {
      // Settled idle is the common case: no State/string copies on every render frame.
      if (pose.position == arrived.position && pose.rotation == arrived.rotation) {
        return latest;
      }
      output = latest;
    } else {
      output = *selected;
      for (const auto& definition : type.fields) {
        if (definition.auth == replication::Authority::Server &&
            definition.id < latest.fields.size() && definition.id < output.fields.size()) {
          output.fields[definition.id] = latest.fields[definition.id];
        }
      }
    }
    output.fields[local::Position] = pose.position;
    output.fields[local::Rotation] = pose.rotation;
    return output;
  }

  void clear() {
    timeline_.clear();
    has_epoch_ = false;
    has_revision_ = false;
  }

 private:
  template <class Value>
  static const Value* field(const State& state, core::FieldId id) {
    return id < state.fields.size() ? std::get_if<Value>(&state.fields[id]) : nullptr;
  }

  static const std::int64_t* clock_of(const State& state) {
    return field<std::int64_t>(state, local::MotionClock);
  }

  static bool same_state(const replication::TypeDef& type, const State& first,
                         const State& second) {
    if (first.type != second.type || first.epoch != second.epoch ||
        first.fields.size() != second.fields.size() || first.fields.size() > type.fields.size()) {
      return false;
    }
    for (std::size_t i = 0; i < first.fields.size(); ++i) {
      if (!replication::same_encoding(type.fields[i], first.fields[i], second.fields[i])) {
        return false;
      }
    }
    return true;
  }

  ReplicaTimeline<State> timeline_;
  std::uint8_t epoch_ = 0;
  bool has_epoch_ = false;
  std::uint64_t revision_ = 0;
  bool has_revision_ = false;
};

}  // namespace kcd2s::integration
