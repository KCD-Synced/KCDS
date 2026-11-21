#include "local_body.hpp"

#include <cmath>
#include <mutex>
#include <string>

namespace kcds::runtime::local_body {
namespace {

namespace sdk = kcd2s::sdk;

std::mutex body_mutex;
std::optional<Body> now;
bool wanted = false;
// Sent once when a worker asks, even unchanged or without a body.
bool unreported = false;
std::optional<Body> reported;
const void* reported_session = nullptr;

bool apart(float a, float b, float step) {
  return std::fabs(a - b) >= step;
}

bool apart(const kcd2s::core::Vec3& a, const kcd2s::core::Vec3& b, float step) {
  return apart(a.x, b.x, step) || apart(a.y, b.y, step) || apart(a.z, b.z, step);
}

sdk::Value numbers(std::initializer_list<float> values) {
  sdk::Value::Array list;
  for (float value : values) {
    list.push_back(sdk::Value(static_cast<double>(value)));
  }
  return sdk::Value(std::move(list));
}

}  // namespace

bool differs(const std::optional<Body>& a, const std::optional<Body>& b) {
  if (a.has_value() != b.has_value()) {
    return true;
  }
  if (!a) {
    return false;
  }
  const kcd2s::core::Quat& qa = a->rotation;
  const kcd2s::core::Quat& qb = b->rotation;
  if (apart(a->position, b->position, 0.001f) || apart(a->velocity, b->velocity, 0.01f) ||
      apart(a->gaze, b->gaze, 0.001f) || apart(qa.x, qb.x, 0.001f) || apart(qa.y, qb.y, 0.001f) ||
      apart(qa.z, qb.z, 0.001f) || apart(qa.w, qb.w, 0.001f)) {
    return true;
  }
  if (a->vitals.has_value() != b->vitals.has_value()) {
    return true;
  }
  if (a->vitals) {
    for (std::size_t index = 0; index < a->vitals->size(); ++index) {
      if (apart((*a->vitals)[index], (*b->vitals)[index], 0.01f)) {
        return true;
      }
    }
  }
  return false;
}

sdk::Value message_of(const std::optional<Body>& body) {
  sdk::Value::Map message{{"op", sdk::Value(std::string("localBody"))}};
  if (body) {
    const Body& b = *body;
    message.emplace_back("p", numbers({b.position.x, b.position.y, b.position.z}));
    message.emplace_back("q", numbers({b.rotation.w, b.rotation.x, b.rotation.y, b.rotation.z}));
    message.emplace_back("v", numbers({b.velocity.x, b.velocity.y, b.velocity.z}));
    message.emplace_back("g", numbers({b.gaze.x, b.gaze.y, b.gaze.z}));
    if (b.vitals) {
      sdk::Value::Array vitals;
      for (float value : *b.vitals) {
        vitals.push_back(sdk::Value(static_cast<double>(value)));
      }
      message.emplace_back("vitals", sdk::Value(std::move(vitals)));
    } else {
      message.emplace_back("vitals", sdk::Value());
    }
  }
  return sdk::Value(std::move(message));
}

void set(const Body& body) {
  std::lock_guard<std::mutex> lock(body_mutex);
  now = body;
}

std::optional<Body> latest() {
  std::lock_guard<std::mutex> lock(body_mutex);
  return now;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (message.tag() != sdk::Tag::Map) {
    return false;
  }
  for (const auto& [key, value] : message.as_map()) {
    if (key == "op" && value.tag() == sdk::Tag::Str && value.as_str() == "localBodyWanted") {
      std::lock_guard<std::mutex> lock(body_mutex);
      wanted = true;
      unreported = true;
      return true;
    }
  }
  return false;
}

void pump(const void* session, const RuntimeSender& send, bool in_world) {
  std::optional<Body> body;
  {
    std::lock_guard<std::mutex> lock(body_mutex);
    if (session != reported_session) {
      // A new runtime asks again when a script of it reads the local player.
      reported_session = session;
      wanted = false;
      unreported = false;
      reported.reset();
    }
    if (!in_world) {
      // The body of a world that ended is no body of the next one.
      now.reset();
    }
    if (!wanted || (!unreported && !differs(now, reported))) {
      return;
    }
    body = now;
  }
  if (send(message_of(body))) {
    std::lock_guard<std::mutex> lock(body_mutex);
    reported = body;
    unreported = false;
  }
}

}  // namespace kcds::runtime::local_body
