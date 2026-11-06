#include "level_doors.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../doors/doors.hpp"
#include "../gates/gates.hpp"
#include "../level/level_edits.hpp"
#include "kcd2s/integration/gates.hpp"
#include "kcd2s/integration/level_edits.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::level_doors {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr std::size_t kMaxQueued = 64;
// The farthest a gate is looked for around a selection.
constexpr double kMostGateRadius = 60.0;

struct Order {
  std::uint64_t guid = 0;
  bool open = false;
};

struct Question {
  std::int64_t id = 0;
  std::uint64_t guid = 0;
  // localGatesNear: where and how far.
  bool gates = false;
  float at[3] = {};
  float radius = 0.0f;
};

std::mutex queue_mutex;
std::vector<Order> orders;
std::vector<Question> questions;
std::vector<integration::GateState> gate_orders;
// The state each gate was set to here (engine thread); none: the level's own.
std::map<std::uint32_t, integration::GateState> gate_states;
std::map<std::uint32_t, std::chrono::steady_clock::time_point> gate_moved;
std::vector<std::pair<std::int64_t, sdk::Value>> replies;

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

bool flag(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Bool && field->as_bool();
}

double number(const sdk::Value& value, const char* name, double fallback) {
  const sdk::Value* field = integration::script_field(value, name);
  if (!field) {
    return fallback;
  }
  if (field->tag() == sdk::Tag::Double) {
    return field->as_double();
  }
  if (field->tag() == sdk::Tag::Int) {
    return double(field->as_int());
  }
  return fallback;
}

bool question(const sdk::Value& message) {
  const sdk::Value* id = integration::script_field(message, "id");
  const sdk::Value* request = integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request) {
    return false;
  }
  const std::string operation = integration::script_operation(*request);
  Question asked;
  asked.id = id->as_int();
  if (operation == "localDoorState") {
    integration::parse_level_edit_guid(text(*request, "guid"), asked.guid);
  } else if (operation == "localGatesNear") {
    asked.gates = true;
    const sdk::Value* at = integration::script_field(*request, "at");
    if (at && at->tag() == sdk::Tag::Map) {
      asked.at[0] = float(number(*at, "x", 0));
      asked.at[1] = float(number(*at, "y", 0));
      asked.at[2] = float(number(*at, "z", 0));
    }
    const double radius = number(*request, "radius", 20.0);
    asked.radius = float(radius > 0.0 && radius <= kMostGateRadius ? radius : kMostGateRadius);
  } else {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (questions.size() < kMaxQueued) {
    questions.push_back(asked);
  }
  return true;
}

sdk::Value state_value(const doors::PreviewState& state) {
  return sdk::Value(sdk::Value::Map{{"door", sdk::Value(state.door)},
                                    {"open", sdk::Value(state.open)},
                                    {"locked", sdk::Value(state.locked)}});
}

// The names of a gate's states and where it stands; false for no such gate on this map.
bool gate_of(std::uint32_t gate, const std::string& map, std::string& key,
             std::vector<std::string>& names, float where[3]) {
  if (gate < integration::kGates.size()) {
    const integration::GateModel& model = integration::kGates[gate];
    if (model.map != map) {
      return false;
    }
    key = std::string(model.key);
    names = {"closed", "open"};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      where[axis] = model.position[axis];
    }
    return true;
  }
  if (gate >= integration::kGateCount) {
    return false;
  }
  const integration::GateSeries& series =
      integration::kGateSeries[gate - integration::kGates.size()];
  if (series.map != map) {
    return false;
  }
  key = std::string(series.key);
  names.clear();
  for (std::size_t state = 0; state < series.state_count; ++state) {
    names.emplace_back(series.states[state].name);
  }
  for (std::size_t axis = 0; axis < 3; ++axis) {
    where[axis] = series.center[axis];
  }
  return true;
}

sdk::Value gates_near(const Question& asked, const std::string& map) {
  sdk::Value::Array found;
  for (std::uint32_t gate = 0; gate < integration::kGateCount; ++gate) {
    std::string key;
    std::vector<std::string> names;
    float where[3] = {};
    if (!gate_of(gate, map, key, names, where)) {
      continue;
    }
    const float distance = std::hypot(where[0] - asked.at[0], where[1] - asked.at[1]);
    if (distance > asked.radius) {
      continue;
    }
    sdk::Value::Array states;
    for (const std::string& name : names) {
      states.emplace_back(name);
    }
    const auto set = gate_states.find(gate);
    const double state = set != gate_states.end() ? double(set->second.state) : -1.0;
    found.emplace_back(sdk::Value::Map{{"gate", sdk::Value(double(gate))},
                                       {"key", sdk::Value(key)},
                                       {"state", sdk::Value(state)},
                                       {"states", sdk::Value(std::move(states))}});
  }
  return sdk::Value(std::move(found));
}

// The gates' states for their module this frame: every gate set here, with the time since its
// move began (the module takes each state it is given as the server's).
std::vector<integration::GateState> gate_frame_states() {
  std::vector<integration::GateState> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(gate_orders);
  }
  const auto now = std::chrono::steady_clock::now();
  for (const integration::GateState& state : taken) {
    gate_states[state.gate] = state;
    gate_moved[state.gate] = now;
  }
  std::vector<integration::GateState> states;
  for (auto& [gate, state] : gate_states) {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - gate_moved[gate]).count();
    const auto most = std::int64_t(integration::kMaxGateElapsedMs);
    state.elapsed_ms = std::uint32_t(elapsed < most ? elapsed : most);
    states.push_back(state);
  }
  return states;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "syncRequest") {
    return question(message);
  }
  if (operation == "localGate") {
    if (!local_mode::enabled()) {
      return true;
    }
    const double gate = number(message, "gate", -1);
    const double state = number(message, "state", -1);
    if (!(gate >= 0 && gate < double(integration::kGateCount)) || !(state >= 0) ||
        std::size_t(state) >= integration::gate_state_count(std::uint32_t(gate))) {
      return true;
    }
    integration::GateState order;
    order.gate = std::uint32_t(gate);
    order.state = std::uint8_t(state);
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (gate_orders.size() < kMaxQueued) {
      gate_orders.push_back(order);
    }
    return true;
  }
  if (operation != "localDoor") {
    return false;
  }
  if (!local_mode::enabled()) {
    return true;
  }
  Order order;
  if (!integration::parse_level_edit_guid(text(message, "guid"), order.guid)) {
    return true;
  }
  order.open = flag(message, "open");
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (orders.size() < kMaxQueued) {
    orders.push_back(order);
  }
  return true;
}

void frame(bool in_world, std::vector<integration::LevelEditChange>& level_edits) {
  const std::string map = in_world ? local_mode::world().map : std::string();
  if (in_world) {
    gates::frame(map, gate_frame_states(), level_edits);
  }
  std::vector<Order> taken_orders;
  std::vector<Question> taken_questions;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken_orders.swap(orders);
    taken_questions.swap(questions);
  }
  if (taken_orders.empty() && taken_questions.empty()) {
    return;
  }
  std::vector<std::uint64_t> guids;
  for (const Order& order : taken_orders) {
    guids.push_back(order.guid);
  }
  for (const Question& asked : taken_questions) {
    if (!asked.gates) {
      guids.push_back(asked.guid);
    }
  }
  const std::map<std::uint64_t, void*> found =
      in_world ? level::entities_by_guid(guids) : std::map<std::uint64_t, void*>{};
  const auto entity_of = [&found](std::uint64_t guid) -> void* {
    const auto entry = found.find(guid);
    return entry != found.end() ? entry->second : nullptr;
  };
  for (const Order& order : taken_orders) {
    doors::preview(entity_of(order.guid), order.open);
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  for (const Question& asked : taken_questions) {
    if (asked.gates) {
      replies.emplace_back(asked.id, gates_near(asked, map));
    } else {
      replies.emplace_back(asked.id, state_value(doors::preview_state(entity_of(asked.guid))));
    }
  }
}

void reset() {
  gate_states.clear();
  gate_moved.clear();
  std::lock_guard<std::mutex> lock(queue_mutex);
  gate_orders.clear();
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<std::pair<std::int64_t, sdk::Value>> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(replies);
  }
  for (auto& [id, value] : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(id)},
                                    {"value", std::move(value)}}));
  }
}

}  // namespace kcds::runtime::architect::level_doors
