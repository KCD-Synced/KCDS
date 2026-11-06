// Windows before the rest (the engine's slots and reasons).
#include "../core/client_internal.hpp"

#include "studio_game_clip.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../animation/player_animation.hpp"
#include "../animation/upper_body.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_view.hpp"

namespace kcds::runtime::studio::game_clip {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
using Clock = std::chrono::steady_clock;

// The shortest part of a clip that plays (of its whole length, 0 to 1).
constexpr float kShortestPart = 0.005f;
// How often the editor is told where the clip is.
constexpr auto kStateEvery = std::chrono::milliseconds(200);
// How often the log says where the body and its hips are while a clip plays.
constexpr auto kLogEvery = std::chrono::seconds(2);

struct Request {
  std::string path;
  bool loop = true;
  // The part of the clip that plays, of its whole length (0 to 1).
  float from = 0.0f;
  float to = 1.0f;
  // The character stays where it stood: the clip's walking and turning do not move it.
  bool in_place = true;
  // Only the upper body plays it (animation/upper_body.hpp keep_legs): the legs keep the layers
  // under it, the body's own stance or walk; then nothing is kept in place.
  bool top_body = false;
};

std::mutex pending_mutex;
std::optional<Request> pending;
// The two layers a clip plays on by turns (a part looped crossfades from one to the other): the
// script clip layer and the one above the talking layer (13), which nothing else plays on.
constexpr unsigned kFirstLayer = player_animation::kScriptClipLayer;
constexpr unsigned kSecondLayer = 14;
unsigned clip_layer = kFirstLayer;
// The body the studio plays a clip on now (so a stop or a new one ends it there), and its order.
void* playing_on = nullptr;
Request playing_now;
// Where the body stood when the clip started (its world matrix, 3 x 4 rows), kept there in place.
float stood[12] = {};
bool stood_kept = false;
// Where the body stood before the clip started: it goes back there when the clip stops.
float home[12] = {};
bool home_kept = false;
// The clip's length in seconds as measured while it plays (its time against the clock).
float measured_seconds = 0.0f;
// Whether measured_seconds is the game's own length of the clip (not estimated by the clock).
bool length_exact = false;
float last_time = -1.0f;
Clock::time_point last_clock;
Clock::time_point state_due;
Clock::time_point log_due;

bool same_place(const float* a, const float* b) {
  for (int index = 0; index < 12; ++index) {
    if (std::fabs(a[index] - b[index]) > 1e-4f) {
      return false;
    }
  }
  return true;
}

// The body's world matrix (slot 32) and putting it back (SetWorldTM, slot 30), as stage_player.
const float* world_matrix(void* entity) {
  using game_client::slot;
  return slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
}

void set_world_matrix(void* entity, const float* matrix) {
  using game_client::slot;
  slot<void(__fastcall*)(void*, const float*, unsigned)>(entity, 30)(
      entity, matrix, game_client::kScriptSetWorldPosReason);
}

// The hips' way from the body across the ground (x, y), smoothed, and where they were at first.
float hips_drift[2] = {0, 0};
float hips_first[2] = {0, 0};
bool drift_known = false;
Clock::time_point drift_clock;
// The hips' way from the body the frame before, and whether the clip went back since.
float last_off[2] = {0, 0};
bool rebase = false;
// Whether the part's start was just crossfaded into (keep_in_part): no new start for in place.
bool crossfaded = false;

// A cutscene's walking is in its skeleton: the hips go metres from the body while the body's
// entity stays (the executioner's hips 1.9 m off it, the body still). In place, the body
// goes back by the way the hips went from where they were at first, smoothed over kDriftSeconds:
// the walking is gone, a strike's sway stays.
constexpr float kDriftSeconds = 0.5f;

void keep_in_place(void* entity) {
  const float* now = world_matrix(entity);
  float hips[3];
  float turn[4];
  const std::string joint = props::entity::body_joint_name(entity, "Hips");
  if (!now || !stood_kept || !props::entity::joint_pose(entity, joint.c_str(), hips, turn)) {
    return;
  }
  const float off[2] = {hips[0] - now[3], hips[1] - now[7]};
  const Clock::time_point clock = Clock::now();
  if (drift_known && rebase) {
    // The clip went back (its part again, or its end to its start): the hips stay where they
    // are in the world, the body goes under them, and the drift starts again from there.
    stood[3] = now[3] + last_off[0] - off[0];
    stood[7] = now[7] + last_off[1] - off[1];
    drift_known = false;
  }
  rebase = false;
  last_off[0] = off[0];
  last_off[1] = off[1];
  if (!drift_known) {
    hips_first[0] = off[0];
    hips_first[1] = off[1];
    hips_drift[0] = off[0];
    hips_drift[1] = off[1];
    drift_known = true;
  } else {
    const float seconds = std::chrono::duration<float>(clock - drift_clock).count();
    const float share = 1.0f - std::exp(-std::clamp(seconds, 0.0f, 0.5f) / kDriftSeconds);
    for (int axis = 0; axis < 2; ++axis) {
      hips_drift[axis] += (off[axis] - hips_drift[axis]) * share;
    }
  }
  drift_clock = clock;
  float place[12];
  std::memcpy(place, stood, sizeof(place));
  place[3] = stood[3] - (hips_drift[0] - hips_first[0]);
  place[7] = stood[7] - (hips_drift[1] - hips_first[1]);
  if (!same_place(now, place)) {
    set_world_matrix(entity, place);
  }
}

// Plays only the part from..to: past its end it starts again from its start (or holds there).
void keep_in_part(void* entity, float time) {
  const Request& order = playing_now;
  const bool part = order.from > 0.0f || order.to < 1.0f;
  if (!part && order.loop) {
    return;
  }
  if (time >= order.to && order.loop) {
    // Again from the part's start, crossfaded there (a jump of the whole pose to the start's
    // looked like the head teleporting, live: 0.2 m in one frame at the seam). The game does not
    // start a clip again on the layer it plays on (refused), so the next round starts on
    // the other of the two layers, blending in over 0.2 s while this one blends out.
    const unsigned other = clip_layer == kFirstLayer ? kSecondLayer : kFirstLayer;
    const bool again =
        player_animation::start_body_clip(entity, order.path, true, other, order.from);
    if (again) {
      player_animation::stop_body_clip(entity, clip_layer);
      clip_layer = other;
      // The pose blends over: in place goes on smoothly, no new start (that made the hips jump
      // 8 cm twice just after the seam).
      crossfaded = true;
    } else {
      player_animation::set_body_clip_time(entity, clip_layer, order.from);
      game_client::log("STUDIO game clip again from %.3f: jumped (not started on layer %u)",
                       order.from, other);
    }
  } else if (time >= order.to) {
    player_animation::set_body_clip_time(entity, clip_layer, order.to);
  } else if (time + 0.002f < order.from) {
    player_animation::set_body_clip_time(entity, clip_layer, order.from);
  }
}

// The clip's length: how long the clock ran for a step of its time (steps across a loop left out).
void measure(float time) {
  const Clock::time_point clock = Clock::now();
  if (!length_exact && last_time >= 0.0f && time > last_time && time - last_time < 0.2f) {
    const float seconds = std::chrono::duration<float>(clock - last_clock).count();
    const float length = seconds / (time - last_time);
    measured_seconds = measured_seconds <= 0.0f ? length : measured_seconds * 0.9f + length * 0.1f;
  }
  last_time = time;
  last_clock = clock;
}

// A bake (studioGameClipBake): the clip's part recorded as the body's joints turn, for the editor
// to make a studio animation of (Client/resources/studio/clip_bake.cjs), one that plays on players
// as every studio animation does (Animations.define). The joints the body is animated with, legs
// included; each joint's parent is recorded with it (a joint's turn is kept against its parent).
constexpr const char* kBakeJoints[] = {
    "Hips",        "Spine",        "Spine1",    "Spine2",      "Spine3",      "Neck",
    "Head",        "LeftShoulder", "LeftArm",   "LeftForeArm", "LeftHand",    "RightShoulder",
    "RightArm",    "RightForeArm", "RightHand", "LeftUpLeg",   "LeftLeg",     "LeftFoot",
    "LeftToeBase", "RightUpLeg",   "RightLeg",  "RightFoot",   "RightToeBase"};
// At most this many frames a second are recorded, and this many frames go in one message.
constexpr float kBakeRate = 30.0f;
constexpr std::size_t kBakeFramesAPart = 12;

struct Bake {
  // The joints recorded (kBakeJoints the body has, then the parents they need), each one's
  // parent's index among them (-1: the body itself, for the hips) and its id.
  std::vector<std::string> names;
  std::vector<int> parents;
  std::vector<int> ids;
  // Each joint's turn in the body's space before the clip (x, y, z, w each) and where the hips
  // were in the body's space.
  std::vector<float> idle;
  float idle_hips[3] = {0, 0, 0};
  bool idle_known = false;
  // A bake asked for; recording: the part's time recorded so far (of its whole length, 0 to 1
  // of the clip) and where it started.
  bool wanted = false;
  bool recording = false;
  float covered = 0.0f;
  float started_at = 0.0f;
  float last_time = -1.0f;
  float next_sample = 0.0f;
  // The frames: part time in seconds, the hips' place (3), then each joint's turn (4).
  std::vector<float> frames;
};
Bake bake;
std::mutex bake_mutex;
bool bake_asked = false;

// The joint's turn in the body's space (the body's turn taken out); false without it.
bool turn_in_body(void* entity, const char* joint, float out[4]) {
  float place[3];
  float joint_turn[4];
  float body_turn[4];
  const std::string name = props::entity::body_joint_name(entity, joint);
  if (!props::entity::joint_pose(entity, name.c_str(), place, joint_turn)) {
    return false;
  }
  props::entity::world_pose(entity, place, body_turn);
  float back[4];
  pose_math::conjugate(body_turn, back);
  pose_math::multiply(back, joint_turn, out);
  return true;
}

// The hips' place in the body's space; false without them.
bool hips_in_body(void* entity, float out[3]) {
  float hips[3];
  float hips_turn[4];
  float place[3];
  float body_turn[4];
  const std::string name = props::entity::body_joint_name(entity, "Hips");
  if (!props::entity::joint_pose(entity, name.c_str(), hips, hips_turn)) {
    return false;
  }
  props::entity::world_pose(entity, place, body_turn);
  float back[4];
  pose_math::conjugate(body_turn, back);
  const float way[3] = {hips[0] - place[0], hips[1] - place[1], hips[2] - place[2]};
  pose_math::rotate(back, way, out);
  return true;
}

// The joints a bake records, by the body's names: kBakeJoints it has, then each one's parent
// that is not among them, up to the skeleton's root (whose parent is the body: -1). The root's
// own turn is not baked: a cutscene turns and walks the body with it.
void name_bake_joints(void* entity) {
  bake.names.clear();
  bake.parents.clear();
  bake.ids.clear();
  for (const char* wanted : kBakeJoints) {
    const std::string name = props::entity::body_joint_name(entity, wanted);
    const int id = name.empty() ? -1 : props::entity::joint_id(entity, name.c_str());
    if (id >= 0) {
      bake.names.push_back(name);
      bake.ids.push_back(id);
    }
  }
  const std::vector<std::string> all = props::entity::joint_names(entity);
  for (std::size_t index = 0; index < bake.ids.size(); ++index) {
    const int id = bake.ids[index];
    const int parent = props::entity::joint_parent(entity, id);
    const auto known = std::find(bake.ids.begin(), bake.ids.end(), parent);
    if (known != bake.ids.end()) {
      bake.parents.push_back(int(known - bake.ids.begin()));
      continue;
    }
    // A joint in between (a twist or helper joint): recorded too, after the others; the loop
    // comes to it and records its own parent the same way.
    if (parent < 0 || std::size_t(parent) >= all.size()) {
      bake.parents.push_back(-1);
      continue;
    }
    bake.parents.push_back(int(bake.ids.size()));
    bake.names.push_back(all[std::size_t(parent)]);
    bake.ids.push_back(parent);
  }
}

// One frame of every recorded joint's turn in the body's space (x, y, z, w each); false when one
// is missing.
bool joint_turns_now(void* entity, std::vector<float>& out) {
  float place[3];
  float body_turn[4];
  props::entity::world_pose(entity, place, body_turn);
  float back[4];
  pose_math::conjugate(body_turn, back);
  for (const int id : bake.ids) {
    float where[3];
    float turn[4];
    if (!props::entity::joint_pose_by_id(entity, id, where, turn)) {
      return false;
    }
    float in_body[4];
    pose_math::multiply(back, turn, in_body);
    out.insert(out.end(), in_body, in_body + 4);
  }
  return true;
}

// Before a clip starts on a body that plays none: the joints' own turns, for a bake.
void keep_idle_turns(void* entity) {
  name_bake_joints(entity);
  bake.idle.clear();
  bake.idle_known = joint_turns_now(entity, bake.idle) && hips_in_body(entity, bake.idle_hips);
}

sdk::Value numbers_of(const std::vector<float>& values, std::size_t from, std::size_t count) {
  sdk::Value::Array out;
  for (std::size_t index = from; index < from + count && index < values.size(); ++index) {
    out.push_back(sdk::Value(double(std::round(values[index] * 10000.0f) / 10000.0f)));
  }
  return sdk::Value(std::move(out));
}

// The recorded frames to the editor, kBakeFramesAPart in each message; the first one names the
// joints and their idle turns.
void send_bake(const std::function<bool(const sdk::Value&)>& send) {
  const std::size_t per_frame = 4 + 4 * bake.ids.size();
  const std::size_t frames = per_frame ? bake.frames.size() / per_frame : 0;
  const std::size_t parts =
      std::max<std::size_t>(1, (frames + kBakeFramesAPart - 1) / kBakeFramesAPart);
  const float seconds = (playing_now.to - playing_now.from) * measured_seconds;
  for (std::size_t part = 0; part < parts; ++part) {
    sdk::Value::Map message{{"op", sdk::Value("studioGameClipBaked")},
                            {"path", sdk::Value(playing_now.path)},
                            {"part", sdk::Value(double(part))},
                            {"parts", sdk::Value(double(parts))},
                            {"seconds", sdk::Value(double(seconds))}};
    if (part == 0) {
      sdk::Value::Array names;
      sdk::Value::Array parents;
      for (std::size_t index = 0; index < bake.names.size(); ++index) {
        names.push_back(sdk::Value(bake.names[index]));
        parents.push_back(sdk::Value(double(bake.parents[index])));
      }
      message.emplace_back("names", sdk::Value(std::move(names)));
      message.emplace_back("parents", sdk::Value(std::move(parents)));
      message.emplace_back("idle", numbers_of(bake.idle, 0, bake.idle.size()));
      message.emplace_back(
          "idleHips", numbers_of(std::vector<float>(bake.idle_hips, bake.idle_hips + 3), 0, 3));
    }
    message.emplace_back("frames", numbers_of(bake.frames, part * kBakeFramesAPart * per_frame,
                                              kBakeFramesAPart * per_frame));
    send(sdk::Value(std::move(message)));
  }
  game_client::log("STUDIO game clip baked: %zu frames of %zu joints over %.2f s in %zu parts",
                   frames, bake.ids.size(), seconds, parts);
}

// One frame of a bake: it records one whole round of the part from where the clip is.
void record_bake(void* entity, float time, const std::function<bool(const sdk::Value&)>& send) {
  {
    std::lock_guard<std::mutex> lock(bake_mutex);
    if (bake_asked) {
      bake_asked = false;
      bake.wanted = true;
    }
  }
  if (!bake.wanted) {
    return;
  }
  const float part = playing_now.to - playing_now.from;
  if (!bake.idle_known || measured_seconds <= 0.0f || part <= 0.0f) {
    bake.wanted = false;
    game_client::log("STUDIO game clip bake: no idle pose or length known");
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioGameClipBaked")},
                                    {"path", sdk::Value(playing_now.path)},
                                    {"failed", sdk::Value(true)}}));
    return;
  }
  if (!bake.recording) {
    bake.recording = true;
    bake.covered = 0.0f;
    bake.started_at = time - playing_now.from;
    bake.last_time = time;
    bake.next_sample = 0.0f;
    bake.frames.clear();
  } else {
    float step = time - bake.last_time;
    // Back to the part's start (its next round): the step goes on from there.
    if (step < 0.0f) {
      step += part;
    }
    bake.covered += step;
    bake.last_time = time;
  }
  const float seconds = bake.covered * measured_seconds;
  if (seconds + 1e-4f >= bake.next_sample && bake.covered < part) {
    std::vector<float> frame;
    float hips[3];
    if (hips_in_body(entity, hips)) {
      const float at = std::fmod(bake.started_at + bake.covered, part) * measured_seconds;
      frame = {at, hips[0], hips[1], hips[2]};
      if (joint_turns_now(entity, frame)) {
        bake.frames.insert(bake.frames.end(), frame.begin(), frame.end());
      }
    }
    bake.next_sample = seconds + 1.0f / kBakeRate;
  }
  if (bake.covered >= part) {
    bake.wanted = false;
    bake.recording = false;
    send_bake(send);
  }
}

sdk::Value state_of(float time) {
  return sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioGameClipState")},
                                    {"path", sdk::Value(playing_now.path)},
                                    {"time", sdk::Value(double(time))},
                                    {"seconds", sdk::Value(double(measured_seconds))},
                                    {"from", sdk::Value(double(playing_now.from))},
                                    {"to", sdk::Value(double(playing_now.to))},
                                    {"loop", sdk::Value(playing_now.loop)},
                                    {"inPlace", sdk::Value(playing_now.in_place)},
                                    {"topBody", sdk::Value(playing_now.top_body)}});
}

void log_where(void* entity, float time) {
  const float* matrix = world_matrix(entity);
  float hips[3] = {0, 0, 0};
  float turn[4];
  const std::string joint = props::entity::body_joint_name(entity, "Hips");
  const bool found = props::entity::joint_pose(entity, joint.c_str(), hips, turn);
  game_client::log(
      "STUDIO game clip at %.3f of %.2f s: body (%.2f %.2f %.2f), hips %s (%.2f %.2f %.2f)", time,
      measured_seconds, matrix ? matrix[3] : 0.0f, matrix ? matrix[7] : 0.0f,
      matrix ? matrix[11] : 0.0f, found ? joint.c_str() : "(none)", hips[0], hips[1], hips[2]);
}

// The head's turn against the chest, logged when it turns more than kSnap in one frame (the owner
// saw the head jump: the game's look IK it was, turned off in the studio's world by
// studio/studio_floor.cpp; this keeps telling if a clip's head still jumps).
constexpr float kSnap = 0.766f;  // cos 40 degrees
float head_before[4] = {0, 0, 0, 1};
bool head_known = false;

void forget_head() {
  head_known = false;
}

// The hips' jumps in the world (more than kHipsJump in a frame), logged: a part's seam jumped
// 0.2 m before it crossfaded.
constexpr float kHipsJump = 0.08f;

void watch_hips(void* entity, float time) {
  static float before[3] = {0, 0, 0};
  static bool known = false;
  float hips[3];
  float turn[4];
  const std::string name = props::entity::body_joint_name(entity, "Hips");
  if (!props::entity::joint_pose(entity, name.c_str(), hips, turn)) {
    known = false;
    return;
  }
  const float moved = std::sqrt((hips[0] - before[0]) * (hips[0] - before[0]) +
                                (hips[1] - before[1]) * (hips[1] - before[1]) +
                                (hips[2] - before[2]) * (hips[2] - before[2]));
  if (known && moved > kHipsJump) {
    game_client::log("STUDIO game clip hips jumped %.3f m in a frame at %.4f", moved, time);
  }
  std::copy(hips, hips + 3, before);
  known = true;
}

float cos_between(const float a[4], const float b[4]) {
  const float dot = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
  return std::clamp(2.0f * dot * dot - 1.0f, -1.0f, 1.0f);
}

void watch_head(void* entity, float time) {
  float place[3];
  float chest[4];
  float head[4];
  const std::string chest_name = props::entity::body_joint_name(entity, "Spine2");
  const std::string head_name = props::entity::body_joint_name(entity, "Head");
  if (!props::entity::joint_pose(entity, chest_name.c_str(), place, chest) ||
      !props::entity::joint_pose(entity, head_name.c_str(), place, head)) {
    head_known = false;
    return;
  }
  float chest_back[4];
  pose_math::conjugate(chest, chest_back);
  float against[4];
  pose_math::multiply(chest_back, head, against);
  if (head_known) {
    const float turned = cos_between(against, head_before);
    if (turned < kSnap) {
      game_client::log("STUDIO game clip head snapped %.0f deg in a frame at %.4f",
                       std::acos(turned) * 57.2958f, time);
    }
  }
  std::copy(against, against + 4, head_before);
  head_known = true;
}

void stop() {
  if (!playing_on) {
    return;
  }
  player_animation::stop_body_clip(playing_on, kFirstLayer);
  player_animation::stop_body_clip(playing_on, kSecondLayer);
  clip_layer = kFirstLayer;
  bake.wanted = false;
  bake.recording = false;
  // Back where it stood before the clip moved it.
  if (home_kept) {
    set_world_matrix(playing_on, home);
  }
  playing_on = nullptr;
  stood_kept = false;
  home_kept = false;
  forget_head();
}

double number_or(const sdk::Value& message, const char* name, double otherwise) {
  const sdk::Value* field = integration::script_field(message, name);
  double value = otherwise;
  return field && view::number_of(*field, value) ? value : otherwise;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "studioGameClipBake") {
    // One round of the part playing, recorded (record_bake).
    std::lock_guard<std::mutex> lock(bake_mutex);
    bake_asked = true;
    return true;
  }
  if (operation != "studioGameClip") {
    return false;
  }
  Request request;
  const sdk::Value* path = integration::script_field(message, "path");
  request.path = path && path->tag() == sdk::Tag::Str ? path->as_str() : std::string();
  const sdk::Value* loop = integration::script_field(message, "loop");
  request.loop = !(loop && loop->tag() == sdk::Tag::Bool && !loop->as_bool());
  const sdk::Value* in_place = integration::script_field(message, "inPlace");
  request.in_place = !(in_place && in_place->tag() == sdk::Tag::Bool && !in_place->as_bool());
  const sdk::Value* top_body = integration::script_field(message, "topBody");
  request.top_body = top_body && top_body->tag() == sdk::Tag::Bool && top_body->as_bool();
  request.from = std::clamp(float(number_or(message, "from", 0.0)), 0.0f, 1.0f - kShortestPart);
  request.to = std::clamp(float(number_or(message, "to", 1.0)), request.from + kShortestPart, 1.0f);
  std::lock_guard<std::mutex> lock(pending_mutex);
  pending = request;
  return true;
}

void tick(void* local_entity, const std::function<bool(const sdk::Value&)>& send) {
  std::optional<Request> now;
  {
    std::lock_guard<std::mutex> lock(pending_mutex);
    now.swap(pending);
  }
  if (now && local_entity) {
    if (now->path.empty()) {
      stop();
      game_client::log("STUDIO game clip stopped");
      send(state_of(-1.0f));
    } else if (playing_on && now->path == playing_now.path) {
      // The same clip with another part, loop or place: it plays on, from the new part's start
      // when it is outside the part.
      if (!now->in_place && stood_kept) {
        stood_kept = false;
      } else if (now->in_place && !stood_kept) {
        if (const float* matrix = world_matrix(local_entity)) {
          std::memcpy(stood, matrix, sizeof(stood));
          stood_kept = true;
          drift_known = false;
        }
      }
      playing_now = *now;
    } else {
      // A new clip takes the layer's place by itself: one stopped and started again in the same
      // frame is refused ("the body did not take it" on the second start).
      if (home_kept && playing_on) {
        set_world_matrix(playing_on, home);
      }
      // The joints' own turns before any clip, for a bake.
      if (!playing_on) {
        keep_idle_turns(local_entity);
      }
      const float* matrix = world_matrix(local_entity);
      home_kept = matrix != nullptr;
      if (home_kept) {
        std::memcpy(home, matrix, sizeof(home));
        std::memcpy(stood, matrix, sizeof(stood));
      }
      stood_kept = now->in_place && home_kept;
      drift_known = false;
      // A part plays on a looping clip (keep_in_part starts it again or holds it at its end).
      const bool part = now->from > 0.0f || now->to < 1.0f;
      // On the layer the clip before played on (it takes its place there).
      const bool started =
          player_animation::start_body_clip(local_entity, now->path, now->loop || part, clip_layer,
                                            now->from > 0.0f ? now->from : -1.0f);
      playing_on = started ? local_entity : nullptr;
      playing_now = *now;
      // Its whole length as the game has it (GetDuration_sec): measured against the clock it
      // was 37 s instead of 31.6 just after the start, and an export's part came out late.
      measured_seconds =
          started ? player_animation::body_clip_seconds(local_entity, now->path) : 0.0f;
      length_exact = measured_seconds > 0.0f;
      last_time = -1.0f;
      forget_head();
      game_client::log("STUDIO game clip %s (%.3f to %.3f, %s): %s", now->path.c_str(), now->from,
                       now->to, now->in_place ? "in place" : "moving",
                       started ? "playing" : "the body did not take it");
    }
  }
  if (!playing_on || playing_on != local_entity) {
    return;
  }
  const float time = player_animation::body_clip_time(local_entity, clip_layer);
  if (time < 0.0f) {
    return;
  }
  // The clip went back (its part again, or its end to its start): in place starts afresh there,
  // unless the part's start was crossfaded into.
  if (last_time >= 0.0f && time + 0.01f < last_time) {
    rebase = !crossfaded;
    crossfaded = false;
  }
  measure(time);
  watch_head(local_entity, time);
  watch_hips(local_entity, time);
  keep_in_part(local_entity, time);
  if (playing_now.top_body) {
    // The legs keep the body's own pose under the clip (as a server's upperBody clip plays).
    kcds::runtime::animation::upper_body::keep_legs(local_entity, clip_layer);
  } else if (stood_kept) {
    keep_in_place(local_entity);
  }
  record_bake(local_entity, time, send);
  const Clock::time_point clock = Clock::now();
  if (clock >= state_due) {
    state_due = clock + kStateEvery;
    send(state_of(time));
  }
  if (clock >= log_due) {
    log_due = clock + kLogEvery;
    log_where(local_entity, time);
  }
}

bool playing() {
  return playing_on != nullptr;
}

}  // namespace kcds::runtime::studio::game_clip
