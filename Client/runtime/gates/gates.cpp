#include "gates.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include "../props/prop_entity.hpp"
#include "series.hpp"

namespace kcds::runtime::gates {
namespace {

using kcd2s::integration::GateModel;
using kcd2s::integration::GateState;
using kcd2s::integration::kGates;
using kcd2s::integration::LevelEditChange;
using kcd2s::integration::LevelEditKind;
using Clock = std::chrono::steady_clock;

// The level edits of the static open meshes, one per gate, above the ids the server gives.
constexpr std::uint32_t kEditBase = 0xFFFFFF00u;
// A model not spawned or a clip not started yet is tried again after this.
constexpr std::chrono::milliseconds kRetry{1000};

Logger log_line = nullptr;

// One gate as this game shows it.
struct Shown {
  bool known = false;  // on this world's level (in the level's state until the server says)
  GateState state;
  Clock::time_point began;  // when its move began here
  void* entity = nullptr;   // its animated model
  std::string_view clip;    // the clip started on it, held (empty: none yet)
  bool mesh_sent = false;   // the level's static open model is taken out
  bool solids_made = false;
  void* open_solid = nullptr;    // the collision while the way is free
  void* closed_solid = nullptr;  // the collision while the gate blocks it
  bool blocking = false;
  bool blocking_known = false;
  Clock::time_point next_try;
};

constexpr char kBlockerMesh[] = "objects/default/primitive_box.cgf";  // a 1 m cube on its centre

std::array<Shown, kGates.size()> shown;

std::string entity_name(const GateModel& model) {
  return "kcds_gate_" + std::string(model.key);
}

std::uint32_t edit_id(std::size_t gate) {
  return kEditBase + std::uint32_t(gate);
}

// The point in the world of a point in the gate model's own axes.
void model_to_world(const GateModel& model, const std::array<float, 3>& local, float world[3]) {
  const float x = model.rotation[0];
  const float y = model.rotation[1];
  const float z = model.rotation[2];
  const float w = model.rotation[3];
  // v + 2w(q x v) + 2q x (q x v)
  const float cx = y * local[2] - z * local[1];
  const float cy = z * local[0] - x * local[2];
  const float cz = x * local[1] - y * local[0];
  world[0] = model.position[0] + local[0] + 2.0f * (w * cx + y * cz - z * cy);
  world[1] = model.position[1] + local[1] + 2.0f * (w * cy + z * cx - x * cz);
  world[2] = model.position[2] + local[2] + 2.0f * (w * cz + x * cy - y * cx);
}

// Spawns (or finds again) one of the gate's unseen solids: a static mesh at the place, turned as
// the gate model, scaled (a box) when `scale` is given; hidden (and so without collision) until
// its state comes.
void* make_solid(const std::string& name, const std::string& mesh, const float place[3],
                 const GateModel& model, const float* scale) {
  void* solid = props::entity::find(name);
  if (!solid) {
    solid = props::entity::spawn_solid_mesh(name, place, mesh);
    if (!solid) {
      if (log_line) {
        log_line("GATES %s: its solid %s could not be spawned", name.c_str(), mesh.c_str());
      }
      return nullptr;
    }
    props::entity::rotate(solid, model.rotation.data());
    if (scale) {
      props::entity::set_scale(solid, scale);
    }
    props::entity::physicalize(solid, props::entity::Physics::Static, 0.0f);
    props::entity::set_opacity(solid, 0.0f);
  }
  props::entity::hide(solid, true);
  return solid;
}

// The gate's collision: the game's own open and closed models (or a box for the closed gate).
void make_solids(Shown& gate, const GateModel& model) {
  if (gate.solids_made) {
    return;
  }
  gate.solids_made = true;
  const float* place = model.open_mesh_position.data();
  if (!model.open_mesh.empty()) {
    gate.open_solid = make_solid(entity_name(model) + "_open", std::string(model.open_mesh), place,
                                 model, nullptr);
  }
  if (!model.closed_mesh.empty()) {
    gate.closed_solid = make_solid(entity_name(model) + "_closed", std::string(model.closed_mesh),
                                   place, model, nullptr);
  } else if (model.blocker_size[0] > 0.0f) {
    float center[3];
    model_to_world(model, model.blocker_center, center);
    gate.closed_solid = make_solid(entity_name(model) + "_block", kBlockerMesh, center, model,
                                   model.blocker_size.data());
  }
  gate.blocking_known = false;
}

// The clip the gate's move plays: the opening one while it opens (or stands open), when it has
// one; otherwise the closing one.
std::string_view clip_of(const Shown& gate, const GateModel& model) {
  return gate.state.open() && !model.open_clip.empty() ? model.open_clip : model.clip;
}

// Spawns (or finds again) the gate's model and starts the move's clip held; false while it
// cannot.
bool make_model(Shown& gate, const GateModel& model) {
  if (!gate.entity) {
    const std::string name = entity_name(model);
    gate.entity = props::entity::find(name);
    if (!gate.entity) {
      const std::string file(model.model);
      gate.entity = props::entity::spawn(name, model.position.data(), file.c_str(), nullptr);
      if (!gate.entity) {
        if (log_line) {
          log_line("GATES %s: its model %s could not be spawned", name.c_str(), file.c_str());
        }
        return false;
      }
    }
    props::entity::rotate(gate.entity, model.rotation.data());
    gate.clip = {};
  }
  make_solids(gate, model);
  const std::string_view wanted = clip_of(gate, model);
  if (gate.clip != wanted) {
    const std::string clip(wanted);
    props::entity::Clip held;
    held.name = clip.c_str();
    held.blend = 0.0f;
    held.speed = 0.0f;
    held.loop = false;
    const bool started = props::entity::play(gate.entity, held);
    gate.clip = started ? wanted : std::string_view();
    if (log_line) {
      log_line("GATES %s: its clip %s %s", std::string(model.key).c_str(), clip.c_str(),
               started ? "started" : "not found yet");
    }
  }
  return !gate.clip.empty();
}

// How far the gate's move has come: 0 when it began, 1 when it is over.
float progress_of(const Shown& gate, const GateModel& model, Clock::time_point now) {
  const float seconds = std::chrono::duration<float>(now - gate.began).count();
  const float length = gate.state.open() ? model.open_seconds : model.seconds;
  const float progress = length > 0.0f ? seconds / length : 1.0f;
  return progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
}

// Takes the level's static open model out of the world for good (the edit undoes itself with
// the world).
void hide_level_mesh(std::size_t index, Shown& gate, const GateModel& model,
                     std::vector<LevelEditChange>& level_edits) {
  if (gate.mesh_sent || model.open_mesh.empty()) {
    return;
  }
  LevelEditChange change;
  change.edit.id = edit_id(index);
  change.edit.kind = LevelEditKind::Brush;
  change.edit.name = std::string(model.open_mesh);
  for (int axis = 0; axis < 3; ++axis) {
    change.edit.at[axis] = model.open_mesh_position[std::size_t(axis)];
  }
  change.edit.hidden = true;
  level_edits.push_back(change);
  gate.mesh_sent = true;
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
  series::install(logger);
}

void frame(std::string_view map, const std::vector<GateState>& states,
           std::vector<LevelEditChange>& level_edits) {
  const Clock::time_point now = Clock::now();
  for (std::size_t index = 0; index < shown.size(); ++index) {
    // The gates of this world's level are ours from the start, standing as the level has them
    // (open) until the server says otherwise.
    Shown& gate = shown[index];
    if (!gate.known && kGates[index].map == map) {
      gate.known = true;
      gate.state.gate = std::uint32_t(index);
      gate.state.state = kcd2s::integration::kGateOpen;
      gate.began = now - std::chrono::hours(1);
    }
  }
  std::vector<GateState> series_states;
  for (const GateState& state : states) {
    if (state.gate >= shown.size()) {
      series_states.push_back(state);
      continue;
    }
    if (kGates[state.gate].map != map) {
      continue;
    }
    Shown& gate = shown[state.gate];
    gate.known = true;
    gate.state = state;
    gate.began = now - std::chrono::milliseconds(state.elapsed_ms);
  }
  for (std::size_t index = 0; index < shown.size(); ++index) {
    Shown& gate = shown[index];
    const GateModel& model = kGates[index];
    if (!gate.known) {
      continue;
    }
    hide_level_mesh(index, gate, model, level_edits);
    if (gate.clip != clip_of(gate, model) && now >= gate.next_try && !make_model(gate, model)) {
      gate.next_try = now + kRetry;
    }
    const float progress = progress_of(gate, model, now);
    const bool own_open_clip = gate.state.open() && !model.open_clip.empty();
    if (!gate.clip.empty()) {
      // The closing clip runs backwards to open; an opening clip of its own runs forwards.
      const bool backwards = gate.state.open() && !own_open_clip;
      props::entity::set_time(gate.entity, backwards ? 1.0f - progress : progress);
    }
    // The collision follows the way: the closed model's while the gate blocks it, the open
    // model's while it is free.
    const bool block = gate.state.open() ? progress < model.opening_blocks_until
                                         : progress >= model.closing_blocks_from;
    if (gate.solids_made && (block != gate.blocking || !gate.blocking_known)) {
      if (gate.closed_solid) {
        props::entity::hide(gate.closed_solid, !block);
      }
      if (gate.open_solid) {
        props::entity::hide(gate.open_solid, block);
      }
      gate.blocking = block;
      gate.blocking_known = true;
    }
  }
  series::frame(map, series_states);
}

void reset() {
  shown = {};
  series::reset();
}

}  // namespace kcds::runtime::gates
