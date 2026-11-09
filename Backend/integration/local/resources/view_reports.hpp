#pragma once
// Entity views of the resource host as changed-state deltas.
//
// The resource host needs, per connected peer, the entities that peer may see (its interest set
// after dimension and showOnlyTo filtering) and, per entity, its public record (script id,
// kind, dimension, transform). Sending every peer's whole view every 100 ms costs
// peers x visible entities, which is quadratic in a dense gathering. This tracker remembers what
// the resource host already holds and produces only:
//
//   removed entities  - records no tracked view references any more (sent first),
//   upserts           - entity records that changed since they were last sent, at most once per
//                       report cycle per entity,
//   view deltas       - per peer, the members that entered or left its view, plus its owned
//                       members and local player whenever either changed.
//
// A report cycle spreads the peers over several ticks (slices); begin_cycle() starts a new one.
// Ownership and scope come only from the native server: the resource host cannot submit views.
// Portable so the bookkeeping is covered by the Linux unit tests.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/core/math.hpp"
#include "kcd2s/integration/appearance.hpp"
#include "kcd2s/integration/player_stats.hpp"
#include "kcd2s/integration/posture.hpp"

namespace kcd2s::local::resources {

// The largest view the SDK accepts for one peer (matches the client directory limit).
inline constexpr std::size_t kMaxViewEntities = 4096;

struct ReportedEntity {
  std::uint32_t script_id = 0;  // id scripts use: the network id, a horse's public id
  core::EntityId native_id = core::kInvalidEntity;
  std::string kind;  // "player", "horse" or "entity"
  std::uint32_t dimension = 0;
  core::Vec3 position{};
  core::Quat rotation{};
  // A player's published appearance and disguise ("" for its own body).
  integration::Appearance appearance;
  std::string disguise;
  // Whether the player's body is drawn, as its own client publishes it (Player.setVisible).
  bool visible = true;
  // Whether its helmet's visor is down, as its own client publishes it (Player.visorDown).
  bool visor_closed = false;
  // A player's name, connection slot (65535 for none) and whether its body has a pose and a
  // soul.
  std::string nickname;
  std::uint16_t player_index = 0xFFFF;
  bool ready = false;
  // A player's stats as its own client last published them; none until it did.
  std::optional<integration::PlayerVitals> vitals;
  std::optional<integration::PlayerStatsSample> stats;
  std::optional<integration::PlayerAbilities> abilities;
  // How the player moves and looks: the velocity its own client measured from its
  // positions, the one its animation was driven by, where its head turns, and the public id of
  // the resource horse it rides (0 on foot).
  core::Vec3 velocity{};
  core::Vec3 motion_velocity{};
  core::Vec3 look_direction{};
  std::uint32_t horse_id = 0;
  // How the player's body stands as its own client last published it.
  integration::PlayerPosture posture;
  // What the body wears and holds drawn, as its own client last published them (the "outfit"
  // and "hands" fields).
  std::string outfit;
  std::string hands;

  friend bool operator==(const ReportedEntity& a, const ReportedEntity& b) {
    return a.script_id == b.script_id && a.native_id == b.native_id && a.kind == b.kind &&
           a.dimension == b.dimension && a.position == b.position && a.rotation == b.rotation &&
           a.appearance == b.appearance && a.disguise == b.disguise && a.visible == b.visible &&
           a.visor_closed == b.visor_closed && a.nickname == b.nickname &&
           a.player_index == b.player_index && a.ready == b.ready && a.vitals == b.vitals &&
           a.stats == b.stats && a.abilities == b.abilities && a.velocity == b.velocity &&
           a.motion_velocity == b.motion_velocity && a.look_direction == b.look_direction &&
           a.horse_id == b.horse_id && a.posture == b.posture && a.outfit == b.outfit &&
           a.hands == b.hands;
  }
  friend bool operator!=(const ReportedEntity& a, const ReportedEntity& b) {
    return !(a == b);
  }
};

// One view member: a native entity id and whether the peer owns that entity.
struct ViewMember {
  core::EntityId entity = core::kInvalidEntity;
  bool owned = false;

  friend bool operator==(const ViewMember& a, const ViewMember& b) {
    return a.entity == b.entity && a.owned == b.owned;
  }
};

struct PeerView {
  core::PeerId peer = 0;
  core::EntityId local_player = core::kInvalidEntity;
  std::vector<ViewMember> members;  // any order; sorted by the tracker
};

struct PeerViewDelta {
  core::PeerId peer = 0;
  core::EntityId local_player = core::kInvalidEntity;
  std::vector<core::EntityId> added;    // ascending
  std::vector<core::EntityId> removed;  // ascending
  std::vector<core::EntityId> owned;    // every owned member after the delta, ascending
};

class ViewReportTracker {
 public:
  // Starts a report cycle. Returns the entities whose records the resource host must drop: no
  // tracked view references them any more. They must be sent before any upsert of this tick.
  std::vector<core::EntityId> begin_cycle() {
    cycle_ += 1;
    std::vector<core::EntityId> removed;
    for (auto it = entities_.begin(); it != entities_.end();) {
      if (it->second.references == 0) {
        removed.push_back(it->first);
        it = entities_.erase(it);
      } else {
        ++it;
      }
    }
    std::sort(removed.begin(), removed.end());
    return removed;
  }

  // True when the entity's record was not refreshed during the current cycle yet. Lets the caller
  // skip reading an entity that another peer of this cycle already reported.
  // The known entities due in this slice of the cycle (their id modulo `slices`) and not yet
  // refreshed in it. Refreshed by their slice, a crowd's records spread over the cycle; refreshed
  // by the first view that held them, every record of a crowd came in the cycle's first slice.
  std::vector<core::EntityId> due_in_slice(unsigned slices, unsigned slice) const {
    std::vector<core::EntityId> due;
    for (const auto& [entity, tracked] : entities_) {
      if (slices != 0 && entity % slices == slice && tracked.refreshed_cycle != cycle_) {
        due.push_back(entity);
      }
    }
    std::sort(due.begin(), due.end());
    return due;
  }

  bool needs_refresh(core::EntityId entity) const {
    auto found = entities_.find(entity);
    if (found == entities_.end()) {
      return true;
    }
    return found->second.refreshed_cycle != cycle_;
  }

  // Records the current state of a visible entity. Returns true when it differs from what the
  // resource host holds, so the caller must send it as an upsert.
  bool refresh(const ReportedEntity& current) {
    auto found = entities_.find(current.native_id);
    if (found == entities_.end()) {
      Tracked tracked;
      tracked.record = current;
      tracked.refreshed_cycle = cycle_;
      entities_.emplace(current.native_id, std::move(tracked));
      return true;
    }
    found->second.refreshed_cycle = cycle_;
    if (found->second.record == current) {
      return false;
    }
    found->second.record = current;
    return true;
  }

  // True when the resource host holds a record of this entity.
  bool known(core::EntityId entity) const {
    return entities_.count(entity) != 0;
  }

  // Compares a peer's current view with the last one sent. Every member must have been refreshed
  // first. Returns false when nothing changed; otherwise fills `delta`.
  bool update_view(PeerView view, PeerViewDelta& delta) {
    std::sort(view.members.begin(), view.members.end(),
              [](const ViewMember& a, const ViewMember& b) { return a.entity < b.entity; });
    auto found = peers_.find(view.peer);
    const bool first = found == peers_.end();
    if (first) {
      found = peers_.emplace(view.peer, PeerState{}).first;
    }
    PeerState& state = found->second;
    if (!first && state.local_player == view.local_player && state.members == view.members) {
      return false;
    }

    delta = PeerViewDelta{};
    delta.peer = view.peer;
    delta.local_player = view.local_player;
    bool owners_changed = first;
    std::size_t old_index = 0;
    std::size_t new_index = 0;
    while (old_index < state.members.size() || new_index < view.members.size()) {
      const bool old_left = old_index < state.members.size();
      const bool new_left = new_index < view.members.size();
      if (old_left &&
          (!new_left || state.members[old_index].entity < view.members[new_index].entity)) {
        delta.removed.push_back(state.members[old_index].entity);
        release(state.members[old_index].entity);
        if (state.members[old_index].owned) {
          owners_changed = true;
        }
        old_index += 1;
      } else if (new_left &&
                 (!old_left || view.members[new_index].entity < state.members[old_index].entity)) {
        delta.added.push_back(view.members[new_index].entity);
        retain(view.members[new_index].entity);
        if (view.members[new_index].owned) {
          owners_changed = true;
        }
        new_index += 1;
      } else {
        if (state.members[old_index].owned != view.members[new_index].owned) {
          owners_changed = true;
        }
        old_index += 1;
        new_index += 1;
      }
    }
    for (const ViewMember& member : view.members) {
      if (member.owned) {
        delta.owned.push_back(member.entity);
      }
    }
    const bool local_changed = first || state.local_player != view.local_player;
    state.local_player = view.local_player;
    state.members = std::move(view.members);
    return first || local_changed || owners_changed || !delta.added.empty() ||
           !delta.removed.empty();
  }

  // The peer left: its members no longer keep records alive. The resource host forgets the peer
  // through its own disconnect message.
  void forget_peer(core::PeerId peer) {
    auto found = peers_.find(peer);
    if (found == peers_.end()) {
      return;
    }
    for (const ViewMember& member : found->second.members) {
      release(member.entity);
    }
    peers_.erase(found);
  }

  // A new resource host holds nothing: the next cycle reports everything again.
  void reset() {
    entities_.clear();
    peers_.clear();
    cycle_ = 0;
  }

  std::size_t tracked_entities() const {
    return entities_.size();
  }
  std::size_t tracked_peers() const {
    return peers_.size();
  }
  // Number of tracked views that contain the entity (0 for unknown entities).
  std::size_t references(core::EntityId entity) const {
    auto found = entities_.find(entity);
    if (found == entities_.end()) {
      return 0;
    }
    return found->second.references;
  }

 private:
  struct Tracked {
    ReportedEntity record;
    std::size_t references = 0;
    std::uint64_t refreshed_cycle = 0;
  };
  struct PeerState {
    core::EntityId local_player = core::kInvalidEntity;
    std::vector<ViewMember> members;  // ascending by entity
  };

  void retain(core::EntityId entity) {
    auto found = entities_.find(entity);
    if (found != entities_.end()) {
      found->second.references += 1;
    }
  }
  void release(core::EntityId entity) {
    auto found = entities_.find(entity);
    if (found != entities_.end() && found->second.references > 0) {
      found->second.references -= 1;
    }
  }

  std::unordered_map<core::EntityId, Tracked> entities_;
  std::unordered_map<core::PeerId, PeerState> peers_;
  std::uint64_t cycle_ = 0;
};

}  // namespace kcd2s::local::resources
