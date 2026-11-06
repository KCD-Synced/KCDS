#include "level_sounds.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "../audio/resource_audio.hpp"
#include "../core/log.hpp"
#include "../native_ui/native_ui.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::level_sounds {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr char kWorldSounds[] = "Libs/GameAudio/world.xml";
// Placed sounds kept at once, and coordinates a sound may be placed at.
constexpr std::size_t kMostSounds = 256;
constexpr double kMostCoordinate = 16384.0;
constexpr std::size_t kMostQueued = 512;

struct Order {
  std::string op;
  std::int64_t id = 0;
  std::string trigger;
  float at[3] = {};
};

std::mutex queue_mutex;
std::vector<Order> orders;
std::vector<std::int64_t> asked;
std::vector<std::pair<std::int64_t, sdk::Value>> replies;
// The placed sounds by the editor's id (engine thread).
std::map<std::int64_t, void*> playing;
// The world's sounds, read once.
sdk::Value listed;
bool read = false;

std::string text(const sdk::Value& message, const char* name) {
  const sdk::Value* value = integration::script_field(message, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string();
}

bool number(const sdk::Value& message, const char* name, double& out) {
  const sdk::Value* value = integration::script_field(message, name);
  if (value && value->tag() == sdk::Tag::Int) {
    out = double(value->as_int());
  } else if (value && value->tag() == sdk::Tag::Double) {
    out = value->as_double();
  } else {
    return false;
  }
  return std::isfinite(out);
}

bool point(const sdk::Value& message, float out[3]) {
  const sdk::Value* at = integration::script_field(message, "at");
  if (!at) {
    return false;
  }
  const char* axes[3] = {"x", "y", "z"};
  for (int axis = 0; axis < 3; ++axis) {
    double value = 0;
    if (!number(*at, axes[axis], value) || std::fabs(value) > kMostCoordinate) {
      return false;
    }
    out[axis] = float(value);
  }
  return true;
}

// A loop or an ambience by its name (the game names them a_l_..., amb_..., ..._loop).
bool ambient(const std::string& name) {
  return name.rfind("a_l_", 0) == 0 || name.rfind("amb_", 0) == 0 ||
         name.find("_loop") != std::string::npos || name.find("_amb") != std::string::npos;
}

// The world's loops and ambiences from its audio controls: [{ name, group }].
sdk::Value world_sounds() {
  std::vector<std::uint8_t> bytes;
  sdk::Value::Array found;
  if (!native_ui::read_game_data(kWorldSounds, bytes)) {
    kcds::game_client::log("ARCHITECT SOUNDS %s not read", kWorldSounds);
    return sdk::Value(std::move(found));
  }
  const std::string xml(bytes.begin(), bytes.end());
  const std::regex trigger("<ATLTrigger atl_name=\"([^\"]+)\"(?: path=\"([^\"]*)\")?");
  for (std::sregex_iterator it(xml.begin(), xml.end(), trigger), end; it != end; ++it) {
    const std::string name = (*it)[1].str();
    if (!ambient(name)) {
      continue;
    }
    found.push_back(sdk::Value(
        sdk::Value::Map{{"name", sdk::Value(name)}, {"group", sdk::Value((*it)[2].str())}}));
  }
  return sdk::Value(std::move(found));
}

void stop(std::int64_t id) {
  auto it = playing.find(id);
  if (it != playing.end()) {
    audio::stop_loop(it->second);
    playing.erase(it);
  }
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string op = integration::script_operation(message);
  if (op == "syncRequest") {
    const sdk::Value* id = integration::script_field(message, "id");
    const sdk::Value* request = integration::script_field(message, "request");
    if (!id || id->tag() != sdk::Tag::Int || !request ||
        integration::script_operation(*request) != "localSoundList") {
      return false;
    }
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (asked.size() < kMostQueued) {
      asked.push_back(id->as_int());
    }
    return true;
  }
  if (op != "localSoundStart" && op != "localSoundMove" && op != "localSoundStop") {
    return false;
  }
  Order order;
  order.op = op;
  double id = 0;
  if (!local_mode::enabled() || !number(message, "id", id)) {
    return true;
  }
  order.id = std::int64_t(id);
  if (op != "localSoundStop" && !point(message, order.at)) {
    return true;
  }
  order.trigger = text(message, "trigger");
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (orders.size() < kMostQueued) {
    orders.push_back(std::move(order));
  }
  return true;
}

void frame(bool in_world) {
  if (!in_world) {
    return;
  }
  std::vector<Order> taken;
  std::vector<std::int64_t> questions;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(orders);
    questions.swap(asked);
  }
  for (const Order& order : taken) {
    if (order.op == "localSoundStop") {
      stop(order.id);
    } else if (order.op == "localSoundMove") {
      auto it = playing.find(order.id);
      if (it != playing.end()) {
        audio::move_loop(it->second, order.at);
      }
    } else if (playing.size() < kMostSounds || playing.count(order.id) != 0) {
      stop(order.id);
      void* loop = audio::start_loop(order.trigger.c_str(), order.at);
      if (loop) {
        playing[order.id] = loop;
      }
      kcds::game_client::log("ARCHITECT SOUND %lld %s %s at %.1f %.1f %.1f",
                             static_cast<long long>(order.id), order.trigger.c_str(),
                             loop ? "playing" : "NOT played (unknown)", order.at[0], order.at[1],
                             order.at[2]);
    }
  }
  if (questions.empty()) {
    return;
  }
  if (!read) {
    read = true;
    listed = world_sounds();
    kcds::game_client::log("ARCHITECT SOUNDS %zu world sounds listed from %s",
                           listed.as_array().size(), kWorldSounds);
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  for (const std::int64_t id : questions) {
    replies.emplace_back(id, listed);
  }
}

void reset() {
  for (const auto& [id, loop] : playing) {
    audio::stop_loop(loop);
  }
  playing.clear();
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

}  // namespace kcds::runtime::architect::level_sounds
