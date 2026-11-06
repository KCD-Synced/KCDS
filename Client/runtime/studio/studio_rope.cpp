// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "studio_rope.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "../core/log.hpp"
#include "../physical_props/rope_prop.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_hold.hpp"
#include "studio_whip.hpp"

namespace kcds::runtime::studio::rope {
namespace {

using physical_props::kChainMaterial;
// Where a new rope starts until the hand's place comes, the middle of the studio's floor.
constexpr float kSpawn[3] = {32.0f, 32.0f, 33.5f};
// The whip test: the start goes round a circle fast instead of following the hand.
constexpr float kSweepCentre[3] = {32.0f, 32.0f, 33.5f};
constexpr float kSweepRadius = 0.8f;
constexpr float kSweepTurnsPerSecond = 1.5f;

// The studio's one rope (kept for the game's life: its node must not go at the process's end).
physical_props::RopeProp& the_rope() {
  static auto* rope = new physical_props::RopeProp();
  return *rope;
}

bool sweeping = false;
// The rope's end pinned to a world point (studioRopeEnd), or free.
std::mutex end_mutex;
bool end_pinned = false;
float end_place[3] = {0, 0, 0};
// The editor's messages, from the network thread to the engine thread.
std::mutex pending_mutex;
std::vector<std::string> pending;

// Recording the simulation into keys and playing them back.
constexpr double kRecordRate = 30.0;
constexpr double kMostRecordSeconds = 10.0;
constexpr std::size_t kMostPlayFrames = std::size_t(kRecordRate * kMostRecordSeconds) + 1;
std::mutex keys_mutex;
// A recording asked for (seconds; 0 for none), taken by the engine thread.
double record_asked = 0.0;
// The keys to play (empty: the rope simulates), and whether they changed.
std::vector<std::vector<float>> play_frames;
double play_rate = kRecordRate;
bool play_changed = false;
// Keys coming in parts (studioRopePlay part, parts), and how many keys a part of the recording
// sent to the editor has (30 keys of a 64-point rope: about 15 kB).
std::vector<std::vector<float>> arriving_frames;
constexpr std::size_t kFramesAPart = 30;
// The engine thread's own: the recording going on and the playback.
struct Recording {
  bool on = false;
  double seconds = 0.0;
  double started = 0.0;
  std::vector<std::vector<float>> frames;
  // A whip's handle frame with each key: its long side and up side (x, y, z each).
  std::vector<std::array<float, 6>> handles;
};
Recording recording;
std::vector<std::vector<float>> playing;
double playing_rate = kRecordRate;
double playing_since = 0.0;

double seconds_now() {
  return GetTickCount64() / 1000.0;
}

// A number rounded to millimetres for the editor (the keys stay small).
kcd2s::sdk::Value metres(float value) {
  return kcd2s::sdk::Value(std::round(double(value) * 1000.0) / 1000.0);
}

// The keys of a { frames: [[x, y, z, ...], ...] } message; empty for none or a bad one.
std::vector<std::vector<float>> frames_of(const kcd2s::sdk::Value* field) {
  std::vector<std::vector<float>> frames;
  if (!field || field->tag() != kcd2s::sdk::Tag::Array ||
      field->as_array().size() > kMostPlayFrames) {
    return frames;
  }
  for (const kcd2s::sdk::Value& frame : field->as_array()) {
    if (frame.tag() != kcd2s::sdk::Tag::Array || frame.as_array().empty() ||
        frame.as_array().size() % 3 != 0) {
      return {};
    }
    std::vector<float> points;
    for (const kcd2s::sdk::Value& part : frame.as_array()) {
      if (part.tag() == kcd2s::sdk::Tag::Double) {
        points.push_back(float(part.as_double()));
      } else if (part.tag() == kcd2s::sdk::Tag::Int) {
        points.push_back(float(part.as_int()));
      } else {
        return {};
      }
    }
    frames.push_back(std::move(points));
  }
  return frames;
}

// A recording's next sample and, when it is over, the keys to the editor.
void record_step(const std::function<bool(const kcd2s::sdk::Value&)>& send) {
  const double now = seconds_now();
  const double elapsed = now - recording.started;
  const std::size_t due = std::size_t(elapsed * kRecordRate) + 1;
  if (recording.frames.size() < due && elapsed <= recording.seconds) {
    // The rope's points, or a whip's lash (the studio has one prop at a time).
    std::vector<float> points = the_rope().made() ? the_rope().points() : whip::lash_points();
    if (!points.empty()) {
      recording.frames.push_back(std::move(points));
      // A whip's handle frame with it (the editor's Lash to keys fits posed-lash keys to it).
      float along[3];
      float up[3];
      if (!the_rope().made() && whip::handle_frame(along, up)) {
        recording.handles.push_back({along[0], along[1], along[2], up[0], up[1], up[2]});
      }
    }
  }
  if (elapsed < recording.seconds) {
    return;
  }
  recording.on = false;
  game_client::log("STUDIO rope recorded: %zu keys of %zu points in %.1f s",
                   recording.frames.size(),
                   recording.frames.empty() ? std::size_t(0) : recording.frames[0].size() / 3,
                   recording.seconds);
  // In parts of kFramesAPart keys: a whole recording in one message was lost past the
  // messages' size (8 s of a whip's lash, 241 keys of 21 points, never came).
  const std::size_t count = recording.frames.size();
  const std::size_t parts = std::max<std::size_t>(1, (count + kFramesAPart - 1) / kFramesAPart);
  for (std::size_t part = 0; part < parts; ++part) {
    kcd2s::sdk::Value::Array frames;
    for (std::size_t index = part * kFramesAPart;
         index < std::min(count, (part + 1) * kFramesAPart); ++index) {
      kcd2s::sdk::Value::Array frame;
      for (const float point : recording.frames[index]) {
        frame.push_back(metres(point));
      }
      frames.push_back(kcd2s::sdk::Value(std::move(frame)));
    }
    kcd2s::sdk::Value::Map message{{"op", kcd2s::sdk::Value("studioRopeRecorded")},
                                   {"rate", kcd2s::sdk::Value(kRecordRate)},
                                   {"part", kcd2s::sdk::Value(std::int64_t(part))},
                                   {"parts", kcd2s::sdk::Value(std::int64_t(parts))},
                                   {"frames", kcd2s::sdk::Value(std::move(frames))}};
    // A whip's handle frames with its keys, when each key has one.
    if (recording.handles.size() == count) {
      kcd2s::sdk::Value::Array handles;
      for (std::size_t index = part * kFramesAPart;
           index < std::min(count, (part + 1) * kFramesAPart); ++index) {
        kcd2s::sdk::Value::Array frame;
        for (const float part_of : recording.handles[index]) {
          frame.push_back(kcd2s::sdk::Value(std::round(double(part_of) * 10000.0) / 10000.0));
        }
        handles.push_back(kcd2s::sdk::Value(std::move(frame)));
      }
      message.emplace_back("handles", kcd2s::sdk::Value(std::move(handles)));
    }
    // The character's forward and up (a whip's lash made from the motion is kept in its frame).
    float forward[3];
    float body_up[3];
    if (!the_rope().made() && whip::body_frame(forward, body_up)) {
      kcd2s::sdk::Value::Array body;
      for (const float part_of : forward) {
        body.push_back(kcd2s::sdk::Value(std::round(double(part_of) * 10000.0) / 10000.0));
      }
      for (const float part_of : body_up) {
        body.push_back(kcd2s::sdk::Value(std::round(double(part_of) * 10000.0) / 10000.0));
      }
      message.emplace_back("body", kcd2s::sdk::Value(std::move(body)));
    }
    send(kcd2s::sdk::Value(std::move(message)));
  }
  recording.frames.clear();
  recording.handles.clear();
}

double number(const kcd2s::sdk::Value& message, const char* name, double fallback) {
  const kcd2s::sdk::Value* field = kcd2s::integration::script_field(message, name);
  if (!field) {
    return fallback;
  }
  if (field->tag() == kcd2s::sdk::Tag::Double) {
    return field->as_double();
  }
  if (field->tag() == kcd2s::sdk::Tag::Int) {
    return double(field->as_int());
  }
  return fallback;
}

}  // namespace

void command(const std::string& line) {
  int segments = 0;
  float length = 0;
  if (line == "rope off") {
    if (the_rope().made()) {
      the_rope().remove();
      game_client::log("STUDIO rope removed");
    }
    return;
  }
  if (line == "rope sweep" || line == "rope hand") {
    sweeping = line == "rope sweep";
    game_client::log("STUDIO rope start %s", sweeping ? "sweeps a circle" : "follows the hand");
    return;
  }
  char look[16] = {};
  const int read = std::sscanf(line.c_str(), "rope %d %f %15s", &segments, &length, look);
  if (read < 2 || segments < 2 || segments > 64 || length < 0.2f || length > 20.0f) {
    game_client::log("STUDIO rope: bad line '%s'", line.c_str());
    return;
  }
  // "chain": the game's chain material on the same rope physics.
  const bool chain = std::string(look) == "chain";
  if (the_rope().make(segments, length, kSpawn, chain ? kChainMaterial : nullptr)) {
    game_client::log("STUDIO %s made: %d segments, %.2f m", chain ? "chain" : "rope", segments,
                     length);
  }
}

bool handle_runtime_message(const kcd2s::sdk::Value& message) {
  namespace integration = kcd2s::integration;
  const std::string operation = integration::script_operation(message);
  if (operation == "studioPropPhysics") {
    // How the rope and the whip's lash move (physical_props/rope_prop.hpp RopePhysics).
    physical_props::RopePhysics physics;
    physics.gravity = float(number(message, "gravity", physics.gravity));
    physics.damping = float(number(message, "damping", physics.damping));
    physics.mass = float(number(message, "mass", physics.mass));
    physics.air_resistance = float(number(message, "air", physics.air_resistance));
    physics.bend_limit = float(number(message, "bend", physics.bend_limit));
    physics.wind = float(number(message, "wind", physics.wind));
    physics.stiffness = float(number(message, "stiffness", physics.stiffness));
    physics.friction = float(number(message, "friction", physics.friction));
    physics.thickness = float(number(message, "thickness", physics.thickness));
    // 1 bodies, props and the floor; 2 the floor only; 0 nothing.
    physics.collide = std::clamp(int(number(message, "collide", 1.0)), 0, 2);
    // A whip's lash: its posed shape (0 physics, 1 the shape) and that shape's bend.
    physics.shape = std::clamp(float(number(message, "shape", 0.0)), 0.0f, 1.0f);
    physics.curl = std::clamp(float(number(message, "curl", 0.0)), -540.0f, 540.0f);
    physics.sway = std::clamp(float(number(message, "sway", 0.0)), -540.0f, 540.0f);
    physical_props::set_rope_physics(physics);
    return true;
  }
  if (operation == "studioRopeEnd") {
    // { on, at: [x, y, z] }: the end tied to the world there, or (on false) free.
    const kcd2s::sdk::Value* on = integration::script_field(message, "on");
    const kcd2s::sdk::Value* at = integration::script_field(message, "at");
    std::lock_guard<std::mutex> lock(end_mutex);
    end_pinned = on && on->tag() == kcd2s::sdk::Tag::Bool && on->as_bool();
    if (at && at->tag() == kcd2s::sdk::Tag::Array && at->as_array().size() == 3) {
      for (std::size_t axis = 0; axis < 3; ++axis) {
        const kcd2s::sdk::Value& part = at->as_array()[axis];
        end_place[axis] = part.tag() == kcd2s::sdk::Tag::Double ? float(part.as_double())
                          : part.tag() == kcd2s::sdk::Tag::Int  ? float(part.as_int())
                                                                : end_place[axis];
      }
    }
    return true;
  }
  if (operation == "studioRopeRecord") {
    // { seconds }: the simulation recorded into keys, 30 a second.
    const double seconds = number(message, "seconds", 3.0);
    std::lock_guard<std::mutex> lock(keys_mutex);
    record_asked = std::clamp(seconds, 0.5, kMostRecordSeconds);
    return true;
  }
  if (operation == "studioRopePlay") {
    // { rate, frames, part?, parts? }: the keys played back in a loop instead of the simulation;
    // no frames stops it. Long keys come in parts (part 0 first; the last one plays them).
    std::vector<std::vector<float>> frames =
        frames_of(integration::script_field(message, "frames"));
    const int part = int(number(message, "part", 0));
    const int parts = std::max(1, int(number(message, "parts", 1)));
    std::lock_guard<std::mutex> lock(keys_mutex);
    if (part == 0) {
      arriving_frames.clear();
    }
    for (std::vector<float>& frame : frames) {
      if (arriving_frames.size() < kMostPlayFrames) {
        arriving_frames.push_back(std::move(frame));
      }
    }
    if (part + 1 >= parts) {
      play_frames = std::move(arriving_frames);
      arriving_frames.clear();
      play_rate = std::clamp(number(message, "rate", kRecordRate), 1.0, 120.0);
      play_changed = true;
    }
    return true;
  }
  std::string line;
  if (operation == "studioRope") {
    char text[64] = {};
    const kcd2s::sdk::Value* look = integration::script_field(message, "look");
    const bool chain = look && look->tag() == kcd2s::sdk::Tag::Str && look->as_str() == "chain";
    std::snprintf(text, sizeof(text), "rope %d %.2f%s", int(number(message, "segments", 16)),
                  number(message, "length", 1.2), chain ? " chain" : "");
    line = text;
  } else if (operation == "studioRopeSweep") {
    const kcd2s::sdk::Value* on = integration::script_field(message, "on");
    const bool sweep = on && on->tag() == kcd2s::sdk::Tag::Bool && on->as_bool();
    line = sweep ? "rope sweep" : "rope hand";
  } else if (operation == "studioRopeOff") {
    line = "rope off";
  } else {
    return false;
  }
  std::lock_guard<std::mutex> lock(pending_mutex);
  pending.push_back(line);
  return true;
}

void tick(void* local_entity, const std::function<bool(const kcd2s::sdk::Value&)>& send) {
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(pending_mutex);
    lines.swap(pending);
  }
  for (const std::string& line : lines) {
    command(line);
  }
  {
    std::lock_guard<std::mutex> lock(keys_mutex);
    if (record_asked > 0.0) {
      recording = Recording{};
      recording.on = true;
      recording.seconds = record_asked;
      recording.started = seconds_now();
      record_asked = 0.0;
      game_client::log("STUDIO rope recording %.1f s", recording.seconds);
    }
    if (play_changed) {
      playing = std::move(play_frames);
      play_frames.clear();
      playing_rate = play_rate;
      playing_since = seconds_now();
      play_changed = false;
      game_client::log("STUDIO rope keys %s", playing.empty() ? "stopped" : "playing");
    }
  }
  if (!the_rope().made()) {
    // A whip (studio_whip, which holds its lash itself): its lash is recorded the same way.
    if (recording.on) {
      record_step(send);
    }
    return;
  }
  if (!local_entity) {
    return;
  }
  if (!playing.empty()) {
    // The keys in place of the simulation, looped; the hand does not hold it meanwhile.
    const double elapsed = seconds_now() - playing_since;
    const std::size_t index = std::size_t(elapsed * playing_rate) % playing.size();
    the_rope().set_points(playing[index]);
    return;
  }
  float start[3];
  if (sweeping) {
    const double seconds = GetTickCount64() / 1000.0;
    const double angle = seconds * kSweepTurnsPerSecond * 6.283185307179586;
    start[0] = kSweepCentre[0] + kSweepRadius * float(std::cos(angle));
    start[1] = kSweepCentre[1] + kSweepRadius * float(std::sin(angle));
    start[2] = kSweepCentre[2];
  } else {
    // In the hand (or the joint) the editor chose, or free in the scene (studio_hold).
    float rotation[4];
    if (!hold::where(local_entity, start, rotation)) {
      return;
    }
  }
  float end[3];
  bool pinned = false;
  {
    std::lock_guard<std::mutex> lock(end_mutex);
    pinned = end_pinned;
    std::copy(std::begin(end_place), std::end(end_place), end);
  }
  the_rope().hold(start, pinned ? end : nullptr);
  if (recording.on) {
    record_step(send);
  }
}

bool made() {
  return the_rope().made();
}

}  // namespace kcds::runtime::studio::rope
