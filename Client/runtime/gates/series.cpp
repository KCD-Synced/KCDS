#include "series.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

#include "../level/level_edits.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::runtime::gates::series {
namespace {

using kcd2s::integration::GateSeries;
using kcd2s::integration::GateSeriesState;
using kcd2s::integration::GateState;
using kcd2s::integration::kGates;
using kcd2s::integration::kGateSeries;
using Clock = std::chrono::steady_clock;

// The leaves of a series are looked for this far (metres each way) around its centre.
constexpr float kReach = 25.0f;
// A series none of whose leaves is found yet is looked for again after this, so many times.
constexpr std::chrono::seconds kRetry{2};
constexpr int kMaxTries = 30;
// A leaf in the next state pairs with one of the same mesh in the last state this close.
constexpr float kPairReach = 4.0f;

Logger log_line = nullptr;

// One leaf of a state: where the level has it, and this game's copy.
struct Leaf {
  std::string mesh;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};  // x, y, z, w
  std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
  void* copy = nullptr;
};

// One series as this game shows it.
struct Shown {
  bool looked = false;  // its leaves were looked for and found
  bool taken = false;   // the level's leaves are out and the copies stand
  int tries = 0;
  Clock::time_point next_try;
  std::vector<std::vector<Leaf>> leaves;  // per state
  std::vector<void*> level_brushes;       // the level's shown leaves, taken out
  std::vector<void*> level_entities;
  int level_state = -1;  // the state the level shows, -1 none
  // The state shown, and the move into it.
  int state = -1;
  int from = -1;
  Clock::time_point began;
  bool moving = false;
  std::vector<int> pair_of;  // for each leaf of `state`: its leaf in `from`, or -1
  // A leaf of only one of the two states (a latch bar) slides out along the gate, by this much,
  // when travelling leaves show the gate's line; `vanishing` and `appearing`: there are such
  // leaves leaving and coming.
  bool slides = false;
  std::array<float, 3> slide{};
  bool vanishing = false;
  bool appearing = false;
  // The bar across the gate (GateSeries::bar_state): which leaves of that state are it, whether
  // the server has it out, and its own move out or in while the gate stands.
  std::vector<bool> bar_leaf;
  bool bar_out = false;
  bool bar_was_out = false;  // the bar was out when the running move began
  bool bar_moving = false;
  Clock::time_point bar_began;
  int bar_state = -1;
  bool pending = false;  // the server's state came before the leaves were found
  GateState wanted;
};

// A sliding leaf: lifted out of its brackets this high, then slid this many times the gap
// between the hinges, into the post's socket and the wall beyond, where it comes and goes.
constexpr float kLift = 0.12f;
constexpr float kSlideReach = 1.2f;
// The least opacity a fading leaf is given.
constexpr float kLeastOpacity = 0.02f;
// The parts of a move: the leaving latch first, the leaves' swing, the coming latch last.
constexpr float kLatchPart = 0.45f;
constexpr float kSwingFrom = 0.35f;
constexpr float kSwingUntil = 0.65f;
// The bar taken out or put back while the gate stands: as long as its part of a move.
constexpr float kBarSeconds = kSeriesSeconds * kLatchPart;

std::array<Shown, kGateSeries.size()> shown;

float clamp01(float value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

// Eased in and out.
float smooth(float value) {
  const float x = clamp01(value);
  return x * x * (3.0f - 2.0f * x);
}

// A brush's world matrix (3x4 by rows) as a position, a rotation and a scale.
void decompose(const float m[12], Leaf& leaf) {
  float axes[3][3];
  for (int column = 0; column < 3; ++column) {
    const float x = m[column];
    const float y = m[4 + column];
    const float z = m[8 + column];
    const float length = std::sqrt(x * x + y * y + z * z);
    leaf.scale[std::size_t(column)] = length;
    const float divisor = length > 0.0f ? length : 1.0f;
    axes[column][0] = x / divisor;
    axes[column][1] = y / divisor;
    axes[column][2] = z / divisor;
  }
  leaf.position = {m[3], m[7], m[11]};
  // r[row][column] = axes[column][row]
  const float r00 = axes[0][0], r01 = axes[1][0], r02 = axes[2][0];
  const float r10 = axes[0][1], r11 = axes[1][1], r12 = axes[2][1];
  const float r20 = axes[0][2], r21 = axes[1][2], r22 = axes[2][2];
  const float trace = r00 + r11 + r22;
  float x, y, z, w;
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    w = 0.25f * s;
    x = (r21 - r12) / s;
    y = (r02 - r20) / s;
    z = (r10 - r01) / s;
  } else if (r00 > r11 && r00 > r22) {
    const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
    w = (r21 - r12) / s;
    x = 0.25f * s;
    y = (r01 + r10) / s;
    z = (r02 + r20) / s;
  } else if (r11 > r22) {
    const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
    w = (r02 - r20) / s;
    x = (r01 + r10) / s;
    y = 0.25f * s;
    z = (r12 + r21) / s;
  } else {
    const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
    w = (r10 - r01) / s;
    x = (r02 + r20) / s;
    y = (r12 + r21) / s;
    z = 0.25f * s;
  }
  leaf.rotation = {x, y, z, w};
}

std::array<float, 4> slerp(const std::array<float, 4>& a, std::array<float, 4> b, float t) {
  float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (dot < 0.0f) {
    for (float& value : b) {
      value = -value;
    }
    dot = -dot;
  }
  std::array<float, 4> out{};
  if (dot > 0.9995f) {
    for (std::size_t i = 0; i < 4; ++i) {
      out[i] = a[i] + (b[i] - a[i]) * t;
    }
  } else {
    const float angle = std::acos(dot);
    const float sine = std::sin(angle);
    const float wa = std::sin((1.0f - t) * angle) / sine;
    const float wb = std::sin(t * angle) / sine;
    for (std::size_t i = 0; i < 4; ++i) {
      out[i] = a[i] * wa + b[i] * wb;
    }
  }
  const float length =
      std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
  for (float& value : out) {
    value /= length > 0.0f ? length : 1.0f;
  }
  return out;
}

bool in_state(const GateSeriesState& state, std::uint16_t layer) {
  for (std::size_t i = 0; i < state.layer_count; ++i) {
    if (state.layers[i] == layer) {
      return true;
    }
  }
  return false;
}

// Mesh paths compare without case and with either slash.
std::string mesh_key(const std::string& mesh) {
  std::string key = mesh;
  for (char& c : key) {
    c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  }
  return key;
}

// Finds the leaves of every state around the series; false while there are none.
bool look(Shown& gate, const GateSeries& series) {
  gate.leaves.assign(series.state_count, {});
  gate.level_brushes.clear();
  gate.level_entities.clear();
  gate.level_state = -1;
  bool any = false;
  for (const level::LevelBrush& brush : level::brushes_near(series.center.data(), kReach)) {
    for (std::size_t index = 0; index < series.state_count; ++index) {
      if (brush.layer == 0 || !in_state(series.states[index], brush.layer)) {
        continue;
      }
      Leaf leaf;
      leaf.mesh = mesh_key(brush.mesh);
      decompose(brush.matrix, leaf);
      gate.leaves[index].push_back(std::move(leaf));
      any = true;
      if (!brush.hidden) {
        gate.level_brushes.push_back(brush.brush);
        if (gate.level_state < 0) {
          gate.level_state = int(index);
        }
      }
    }
  }
  std::vector<std::uint64_t> guids;
  for (std::size_t index = 0; index < series.state_count; ++index) {
    const GateSeriesState& state = series.states[index];
    for (std::size_t e = 0; e < state.entity_count; ++e) {
      guids.push_back(state.entities[e].guid);
    }
  }
  const auto entities = level::entities_by_guid(guids);
  for (std::size_t index = 0; index < series.state_count; ++index) {
    const GateSeriesState& state = series.states[index];
    for (std::size_t e = 0; e < state.entity_count; ++e) {
      const auto& entity = state.entities[e];
      Leaf leaf;
      leaf.mesh = mesh_key(std::string(entity.mesh));
      leaf.position = entity.position;
      leaf.rotation = entity.rotation;
      leaf.scale = entity.scale;
      gate.leaves[index].push_back(std::move(leaf));
      any = true;
      const auto found = entities.find(entity.guid);
      if (found != entities.end()) {
        gate.level_entities.push_back(found->second);
        if (gate.level_state < 0) {
          gate.level_state = int(index);
        }
      }
    }
  }
  return any;
}

void place(void* copy, const std::array<float, 3>& position, const std::array<float, 4>& rotation) {
  props::entity::place_rotated(copy, position.data(), rotation.data());
}

void find_bar(Shown& gate, const GateSeries& series);
void show_state(Shown& gate);

// Takes the level's shown leaves out and stands the copies, the state's shown.
void take_over(Shown& gate, const GateSeries& series, std::size_t index) {
  for (void* brush : gate.level_brushes) {
    level::take_out(brush);
  }
  for (void* entity : gate.level_entities) {
    level::hide_entity(entity, true);
  }
  for (std::size_t state = 0; state < gate.leaves.size(); ++state) {
    for (std::size_t leaf_index = 0; leaf_index < gate.leaves[state].size(); ++leaf_index) {
      Leaf& leaf = gate.leaves[state][leaf_index];
      const std::string name = "kcds_gate_series_" + std::to_string(index) + "_" +
                               std::to_string(state) + "_" + std::to_string(leaf_index);
      leaf.copy = props::entity::find(name);
      if (!leaf.copy) {
        leaf.copy = props::entity::spawn_solid_mesh(name, leaf.position.data(), leaf.mesh);
        if (!leaf.copy) {
          continue;
        }
        place(leaf.copy, leaf.position, leaf.rotation);
        if (leaf.scale[0] != 1.0f || leaf.scale[1] != 1.0f || leaf.scale[2] != 1.0f) {
          props::entity::set_scale(leaf.copy, leaf.scale.data());
        }
        props::entity::physicalize(leaf.copy, props::entity::Physics::Static, 0.0f);
      }
    }
  }
  find_bar(gate, series);
  show_state(gate);
  gate.taken = true;
  if (log_line) {
    std::string counts;
    for (std::size_t state = 0; state < gate.leaves.size(); ++state) {
      counts += std::string(series.states[state].name) + "=" +
                std::to_string(gate.leaves[state].size()) + " ";
    }
    log_line("GATES series %s: %s level state %d, shown %d, %zu brushes and %zu entities taken out",
             std::string(series.key).c_str(), counts.c_str(), gate.level_state, gate.state,
             gate.level_brushes.size(), gate.level_entities.size());
  }
}

// Pairs each leaf of the next state with the nearest unpaired one of the same mesh in the last.
void pair_leaves(Shown& gate) {
  const std::vector<Leaf>& to = gate.leaves[std::size_t(gate.state)];
  const std::vector<Leaf>& from = gate.leaves[std::size_t(gate.from)];
  std::vector<bool> used(from.size(), false);
  gate.pair_of.assign(to.size(), -1);
  for (std::size_t t = 0; t < to.size(); ++t) {
    float best = kPairReach * kPairReach;
    for (std::size_t f = 0; f < from.size(); ++f) {
      if (used[f] || from[f].mesh != to[t].mesh) {
        continue;
      }
      float distance = 0.0f;
      for (std::size_t axis = 0; axis < 3; ++axis) {
        const float d = from[f].position[axis] - to[t].position[axis];
        distance += d * d;
      }
      if (distance <= best) {
        best = distance;
        gate.pair_of[t] = int(f);
      }
    }
    if (gate.pair_of[t] >= 0) {
      used[std::size_t(gate.pair_of[t])] = true;
    }
  }
}

bool paired_from(const Shown& gate, std::size_t from_leaf) {
  for (const int pair : gate.pair_of) {
    if (pair == int(from_leaf)) {
      return true;
    }
  }
  return false;
}

// Which leaves of the bar state are the bar: those whose mesh the other state does not have.
void find_bar(Shown& gate, const GateSeries& series) {
  gate.bar_state = series.bar_state;
  gate.bar_leaf.clear();
  if (gate.bar_state < 0 || series.state_count != 2) {
    gate.bar_state = -1;
    return;
  }
  const std::vector<Leaf>& barred = gate.leaves[std::size_t(gate.bar_state)];
  const std::vector<Leaf>& other = gate.leaves[std::size_t(1 - gate.bar_state)];
  for (const Leaf& leaf : barred) {
    bool shared = false;
    for (const Leaf& open_leaf : other) {
      shared = shared || open_leaf.mesh == leaf.mesh;
    }
    gate.bar_leaf.push_back(!shared);
  }
}

bool is_bar(const Shown& gate, int state, std::size_t leaf) {
  return state >= 0 && state == gate.bar_state && leaf < gate.bar_leaf.size() &&
         gate.bar_leaf[leaf];
}

// The line a bar slides along: from the two of those leaves farthest apart (the gate's hinges),
// towards the first, by kSlideReach of the gap.
void set_slide_line(Shown& gate, const std::vector<Leaf>& leaves,
                    const std::vector<std::size_t>& hinges) {
  gate.slides = false;
  float best = 0.0f;
  for (std::size_t a = 0; a < hinges.size(); ++a) {
    for (std::size_t b = a + 1; b < hinges.size(); ++b) {
      const auto& p = leaves[hinges[a]].position;
      const auto& q = leaves[hinges[b]].position;
      const float dx = p[0] - q[0];
      const float dy = p[1] - q[1];
      const float gap = std::sqrt(dx * dx + dy * dy);
      if (gap > best) {
        best = gap;
        gate.slide = {dx * kSlideReach, dy * kSlideReach, 0.0f};
        gate.slides = gap > 0.5f;
      }
    }
  }
}

// Whether the move has leaves of only one state still to come or go (a bar already out stays
// out), and the line they slide along, from the travelling leaves.
void plan_slide(Shown& gate) {
  const std::vector<Leaf>& to = gate.leaves[std::size_t(gate.state)];
  const std::vector<Leaf>& from = gate.leaves[std::size_t(gate.from)];
  gate.appearing = false;
  gate.vanishing = false;
  std::vector<std::size_t> travelling;
  for (std::size_t t = 0; t < to.size(); ++t) {
    if (gate.pair_of[t] >= 0) {
      travelling.push_back(t);
    } else if (!(is_bar(gate, gate.state, t) && gate.bar_out)) {
      gate.appearing = true;
    }
  }
  for (std::size_t f = 0; f < from.size(); ++f) {
    if (!paired_from(gate, f) && !(is_bar(gate, gate.from, f) && gate.bar_was_out)) {
      gate.vanishing = true;
    }
  }
  set_slide_line(gate, to, travelling);
}

// Places a sliding leaf `gone` of its way out (0 in place, 1 in the post's socket): lifted out
// of its brackets first, then slid.
void slide_leaf(const Shown& gate, const Leaf& leaf, float gone) {
  std::array<float, 3> position = leaf.position;
  const float slid = smooth((gone - 0.25f) / 0.75f);
  for (std::size_t axis = 0; axis < 3; ++axis) {
    position[axis] += gate.slide[axis] * slid;
  }
  position[2] += kLift * smooth(gone / 0.25f);
  place(leaf.copy, position, leaf.rotation);
}

// A fading leaf's opacity, set before it is placed again (the renderer takes the alpha with the
// next placement) and never quite 0 (a copy shown at 0 stays unseen).
void fade_leaf(const Leaf& leaf, float opacity) {
  props::entity::set_opacity(leaf.copy, opacity < kLeastOpacity ? kLeastOpacity : opacity);
  place(leaf.copy, leaf.position, leaf.rotation);
}

// Shows the copies of the state the gate stands in (without its bar when it is out).
void show_state(Shown& gate) {
  for (std::size_t s = 0; s < gate.leaves.size(); ++s) {
    for (std::size_t i = 0; i < gate.leaves[s].size(); ++i) {
      Leaf& leaf = gate.leaves[s][i];
      if (leaf.copy) {
        const bool shown_now = int(s) == gate.state && !(is_bar(gate, int(s), i) && gate.bar_out);
        props::entity::hide(leaf.copy, !shown_now);
      }
    }
  }
}

// Ends a bar move where it was going.
void end_bar_move(Shown& gate) {
  if (!gate.bar_moving) {
    return;
  }
  gate.bar_moving = false;
  std::vector<Leaf>& leaves = gate.leaves[std::size_t(gate.bar_state)];
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    if (is_bar(gate, gate.bar_state, i) && leaves[i].copy) {
      fade_leaf(leaves[i], 1.0f);
      props::entity::hide(leaves[i].copy, gate.bar_out);
    }
  }
}

// Starts the bar's move out or in while the gate stands in the bar state.
void begin_bar(Shown& gate, bool out, std::uint32_t elapsed_ms, Clock::time_point now) {
  end_bar_move(gate);
  gate.bar_out = out;
  gate.bar_began = now - std::chrono::milliseconds(elapsed_ms);
  gate.bar_moving = true;
  std::vector<Leaf>& leaves = gate.leaves[std::size_t(gate.bar_state)];
  std::vector<std::size_t> hinges;
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    if (!is_bar(gate, gate.bar_state, i)) {
      hinges.push_back(i);
    } else if (leaves[i].copy) {
      props::entity::hide(leaves[i].copy, false);
    }
  }
  set_slide_line(gate, leaves, hinges);
}

// Starts the move into the server's state, or stands it when there is no state to move from.
void begin(Shown& gate, const GateState& state, Clock::time_point now) {
  const int next = int(state.state);
  if (next == gate.state && !gate.moving) {
    // The gate stands: only its bar may move.
    if (next == gate.bar_state && state.bar_out != gate.bar_out) {
      begin_bar(gate, state.bar_out, state.bar_elapsed_ms, now);
    }
    return;
  }
  end_bar_move(gate);
  gate.bar_was_out = gate.bar_out;
  gate.bar_out = next == gate.bar_state && state.bar_out;
  if (gate.moving) {
    // A move cut short ends where it was going.
    gate.moving = false;
    for (Leaf& leaf : gate.leaves[std::size_t(gate.from)]) {
      if (leaf.copy) {
        props::entity::hide(leaf.copy, true);
        props::entity::set_opacity(leaf.copy, 1.0f);
        place(leaf.copy, leaf.position, leaf.rotation);
      }
    }
  }
  gate.from = gate.state;
  gate.state = next;
  gate.began = now - std::chrono::milliseconds(state.elapsed_ms);
  gate.moving = gate.from >= 0 && gate.from != gate.state;
  if (gate.moving) {
    pair_leaves(gate);
    plan_slide(gate);
    // The leaves of the next state that do not travel come in: from the post's socket, or faded.
    std::vector<Leaf>& to = gate.leaves[std::size_t(gate.state)];
    for (std::size_t t = 0; t < to.size(); ++t) {
      if (gate.pair_of[t] >= 0 || !to[t].copy || (is_bar(gate, gate.state, t) && gate.bar_out)) {
        continue;
      }
      props::entity::hide(to[t].copy, false);
      if (gate.slides) {
        slide_leaf(gate, to[t], 1.0f);
      } else {
        fade_leaf(to[t], 0.0f);
      }
    }
  } else {
    show_state(gate);
  }
}

// One frame of a move: the travelling leaves placed, the others faded; at its end the next
// state's copies stand.
void step(Shown& gate, Clock::time_point now) {
  if (!gate.moving) {
    return;
  }
  const float seconds = std::chrono::duration<float>(now - gate.began).count();
  const float linear = clamp01(seconds / kSeriesSeconds);
  // A latch leaving goes first and one coming last; the leaves swing in the rest of the move.
  const float swing_from = gate.slides && gate.vanishing ? kSwingFrom : 0.0f;
  const float swing_until = gate.slides && gate.appearing ? kSwingUntil : 1.0f;
  const float t = smooth((linear - swing_from) / (swing_until - swing_from));
  const float leaving = clamp01(linear / kLatchPart);  // 0 in place, 1 gone
  const float coming = 1.0f - clamp01((linear - (1.0f - kLatchPart)) / kLatchPart);
  std::vector<Leaf>& from = gate.leaves[std::size_t(gate.from)];
  std::vector<Leaf>& to = gate.leaves[std::size_t(gate.state)];
  const bool over = linear >= 1.0f;
  for (std::size_t t_index = 0; t_index < to.size(); ++t_index) {
    const int pair = gate.pair_of[t_index];
    if (pair < 0) {
      Leaf& leaf = to[t_index];
      if (!leaf.copy || (is_bar(gate, gate.state, t_index) && gate.bar_out)) {
        continue;
      }
      if (over) {
        fade_leaf(leaf, 1.0f);
      } else if (gate.slides) {
        slide_leaf(gate, leaf, coming);
      } else {
        fade_leaf(leaf, t);
      }
      continue;
    }
    Leaf& moving = from[std::size_t(pair)];
    if (!moving.copy) {
      continue;
    }
    if (over) {
      place(moving.copy, moving.position, moving.rotation);
      props::entity::hide(moving.copy, true);
      if (to[t_index].copy) {
        props::entity::hide(to[t_index].copy, false);
      }
      continue;
    }
    std::array<float, 3> position{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      position[axis] =
          moving.position[axis] + (to[t_index].position[axis] - moving.position[axis]) * t;
    }
    place(moving.copy, position, slerp(moving.rotation, to[t_index].rotation, t));
  }
  for (std::size_t f = 0; f < from.size(); ++f) {
    Leaf& leaf = from[f];
    if (paired_from(gate, f) || !leaf.copy || (is_bar(gate, gate.from, f) && gate.bar_was_out)) {
      continue;
    }
    if (over) {
      fade_leaf(leaf, 1.0f);
      props::entity::hide(leaf.copy, true);
    } else if (gate.slides) {
      slide_leaf(gate, leaf, leaving);
    } else {
      fade_leaf(leaf, 1.0f - t);
    }
  }
  if (over) {
    gate.moving = false;
  }
}

// One frame of the bar's own move out or in while the gate stands.
void bar_step(Shown& gate, Clock::time_point now) {
  if (!gate.bar_moving) {
    return;
  }
  const float seconds = std::chrono::duration<float>(now - gate.bar_began).count();
  const float progress = clamp01(seconds / kBarSeconds);
  const float gone = gate.bar_out ? progress : 1.0f - progress;
  std::vector<Leaf>& leaves = gate.leaves[std::size_t(gate.bar_state)];
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    if (!is_bar(gate, gate.bar_state, i) || !leaves[i].copy) {
      continue;
    }
    if (gate.slides) {
      slide_leaf(gate, leaves[i], gone);
    } else {
      fade_leaf(leaves[i], 1.0f - gone);
    }
  }
  if (progress >= 1.0f) {
    end_bar_move(gate);
  }
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
}

void frame(std::string_view map, const std::vector<GateState>& states) {
  const Clock::time_point now = Clock::now();
  for (const GateState& state : states) {
    const std::size_t index = state.gate - kGates.size();
    if (state.gate < kGates.size() || index >= shown.size() || kGateSeries[index].map != map) {
      continue;
    }
    shown[index].pending = true;
    shown[index].wanted = state;
  }
  for (std::size_t index = 0; index < shown.size(); ++index) {
    const GateSeries& series = kGateSeries[index];
    Shown& gate = shown[index];
    if (series.map != map) {
      continue;
    }
    // Looked for again until the level shows one of its states: its layers' entities are spawned
    // some time after the world is ready. A series the level shows in none is taken as it is
    // after the last try (or at once when the server sets its state).
    if (!gate.looked && gate.tries < kMaxTries && now >= gate.next_try) {
      ++gate.tries;
      gate.next_try = now + kRetry;
      const bool any = look(gate, series);
      gate.looked = any && (gate.level_state >= 0 || gate.pending || gate.tries >= kMaxTries);
      if (gate.looked) {
        gate.state = gate.level_state;
      }
    }
    if (!gate.looked) {
      continue;
    }
    // Taken over once the level shows one of its states or the server sets one.
    if (!gate.taken && (gate.state >= 0 || gate.pending)) {
      if (gate.state < 0) {
        gate.state = int(gate.wanted.state);
      }
      take_over(gate, series, index);
    }
    if (gate.taken && gate.pending) {
      gate.pending = false;
      begin(gate, gate.wanted, now);
    }
    step(gate, now);
    bar_step(gate, now);
  }
}

void reset() {
  shown = {};
}

}  // namespace kcds::runtime::gates::series
