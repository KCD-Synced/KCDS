// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "voice_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "../core/log.hpp"
#include "../hud/hud_calls.hpp"
#include "../keybinds/bound_keys.hpp"
#include "../keybinds/keybinds.hpp"
#include "../physics/line_of_sight.hpp"
#include "../props/prop_entity.hpp"
#include "../replicas/remote.hpp"
#include "../world/interior.hpp"
#include "../world/world_weather.hpp"
#include "kcd2s/integration/player_stats.hpp"
#include "kcd2s/integration/replica_buffs.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "kcd2s/integration/voice_messages.hpp"
#include "kcd2s/voice/frame.hpp"
#include "kcds/voice/voice_engine.hpp"
#include "voice_indicator.hpp"

namespace kcds::runtime::voice {
namespace {

using kcds::game_client::log;
using kcds::voice::EngineStats;
using kcds::voice::TalkMode;
using kcds::voice::VoiceEngine;
namespace integration = kcd2s::integration;

constexpr const char* kTalkAction = "kcds_voice_talk";
constexpr const char* kTierAction = "kcds_voice_tier";

std::mutex engine_mutex;
std::unique_ptr<VoiceEngine> engine;
std::atomic<DatagramSender> send_datagram{nullptr};

std::mutex settings_mutex;
Settings current;
bool settings_read = false;

integration::VoiceState state;
bool have_state = false;
kcd2s::voice::KeyPair own_keys;
std::uint32_t keyed_session = 0;
// Talkers the server muffles (a gag), 0 to 1. On the engine thread.
std::map<std::uint32_t, float> muffled;

std::array<std::uint8_t, 32> keyed_server{};
bool tier_key_was_down = false;
std::vector<std::uint32_t> heard_now;
bool was_talking = false;

// Test hooks (KCDS_TEST_HOOKS builds' kits only).
std::thread test_thread;
std::atomic<bool> test_running{false};
std::vector<float> test_speech;
std::size_t test_position = 0;
std::atomic<double> test_heard_energy{0.0};
std::atomic<std::uint64_t> test_heard_samples{0};
std::FILE* test_listen_file = nullptr;

std::string settings_path() {
  const char* base = std::getenv("LOCALAPPDATA");
  std::string path = base ? base : ".";
  return path + "\\KCDS\\voice.json";
}

// A small flat JSON object of this file's own writing: the keys in Settings, one per line.
std::string read_text_field(const std::string& text, const char* name) {
  const std::string key = std::string("\"") + name + "\": \"";
  const auto start = text.find(key);
  if (start == std::string::npos) {
    return {};
  }
  const auto from = start + key.size();
  const auto end = text.find('"', from);
  return end == std::string::npos ? std::string{} : text.substr(from, end - from);
}

float read_number_field(const std::string& text, const char* name, float fallback) {
  const std::string key = std::string("\"") + name + "\": ";
  const auto start = text.find(key);
  if (start == std::string::npos) {
    return fallback;
  }
  const float value = float(std::atof(text.c_str() + start + key.size()));
  return std::isfinite(value) ? value : fallback;
}

// Under settings_mutex.
void read_settings_once() {
  if (settings_read) {
    return;
  }
  settings_read = true;
  std::ifstream file(settings_path());
  if (!file) {
    return;
  }
  std::stringstream text;
  text << file.rdbuf();
  const std::string all = text.str();
  current.voice_activation = read_text_field(all, "mode") == "voice";
  current.input_volume = std::clamp(read_number_field(all, "inputVolume", 1.0f), 0.0f, 4.0f);
  current.output_volume = std::clamp(read_number_field(all, "outputVolume", 1.0f), 0.0f, 2.0f);
  current.input_device = read_text_field(all, "inputDevice");
  current.output_device = read_text_field(all, "outputDevice");
  current.voice_threshold = std::clamp(read_number_field(all, "voiceThreshold", 0.6f), 0.3f, 0.9f);
  current.show_talking_icons = read_number_field(all, "talkingIcons", 1.0f) != 0.0f;
  current.show_own_mic = read_number_field(all, "ownMic", 1.0f) != 0.0f;
  current.echo_cancellation = read_number_field(all, "echoCancellation", 0.0f) != 0.0f;
}

std::string escaped(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20) {
      out += c;
    }
  }
  return out;
}

void write_settings(const Settings& values) {
  const char* base = std::getenv("LOCALAPPDATA");
  if (!base) {
    return;
  }
  CreateDirectoryA((std::string(base) + "\\KCDS").c_str(), nullptr);
  std::ofstream file(settings_path(), std::ios::trunc);
  file << "{\n  \"mode\": \"" << (values.voice_activation ? "voice" : "push") << "\",\n"
       << "  \"inputVolume\": " << values.input_volume << ",\n"
       << "  \"outputVolume\": " << values.output_volume << ",\n"
       << "  \"inputDevice\": \"" << escaped(values.input_device) << "\",\n"
       << "  \"outputDevice\": \"" << escaped(values.output_device) << "\",\n"
       << "  \"voiceThreshold\": " << values.voice_threshold << ",\n"
       << "  \"talkingIcons\": " << (values.show_talking_icons ? 1 : 0) << ",\n"
       << "  \"ownMic\": " << (values.show_own_mic ? 1 : 0) << ",\n"
       << "  \"echoCancellation\": " << (values.echo_cancellation ? 1 : 0) << "\n}\n";
}

// Under engine_mutex.
void apply_settings(const Settings& values) {
  if (!engine) {
    return;
  }
  engine->set_mode(values.voice_activation ? TalkMode::VoiceActivation : TalkMode::PushToTalk);
  engine->set_input_volume(values.input_volume);
  engine->set_output_volume(values.output_volume);
  engine->set_input_device(values.input_device);
  engine->set_output_device(values.output_device);
  engine->set_voice_threshold(values.voice_threshold);
  engine->set_echo_cancellation(values.echo_cancellation);
}

bool action_held(const char* action) {
  return kcds::runtime::keybinds::held(action);
}

// The push-to-talk key as the indicator shows it: its first binding in capitals, a mouse button
// as M and its number ("mouse4": "M4"); "" when it is unbound.
std::wstring push_to_talk_key() {
  const std::vector<std::string> keys = kcds::runtime::keybinds::keys_of(kTalkAction);
  if (keys.empty() || keys.front().empty()) {
    return {};
  }
  std::string name = keys.front();
  if (name.rfind("mouse", 0) == 0) {
    name = "m" + name.substr(5);
  }
  std::wstring shown;
  for (const char letter : name) {
    shown += wchar_t(std::toupper(static_cast<unsigned char>(letter)));
  }
  return shown;
}

bool game_in_front() {
  HWND window = GetForegroundWindow();
  DWORD process = 0;
  if (window) {
    GetWindowThreadProcessId(window, &process);
  }
  return process == GetCurrentProcessId();
}

// 10 ms of test audio each 10 ms of real time, as a sound card's clock would run it; sleeping 10 ms
// after each step ran about 8 % slow and made a delay measured through it drift.
void test_thread_main() {
  auto next = std::chrono::steady_clock::now();
  while (test_running.load()) {
    {
      std::lock_guard<std::mutex> lock(engine_mutex);
      if (engine) {
        engine->test_step();
      }
    }
    next += std::chrono::milliseconds(10);
    std::this_thread::sleep_until(next);
  }
}

// Kit tests only: a recording of a synthetic voice in place of the microphone, and a measure of
// what comes out. KCDS_TEST_VOICE_LISTEN_FILE (to hear and time the test sound)
// also writes what this game hears to that file, as 48 kHz stereo floats. A player's game never has
// it.
void start_test_devices_if_asked() {
  const char* path = std::getenv("KCDS_TEST_VOICE_FILE");
  const char* listen = std::getenv("KCDS_TEST_VOICE_LISTEN");
  if ((!path && !listen) || test_running.load()) {
    return;
  }
  if (path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto bytes = file ? static_cast<std::size_t>(file.tellg()) : 0;
    test_speech.resize(bytes / sizeof(float));
    if (bytes != 0) {
      file.seekg(0);
      file.read(reinterpret_cast<char*>(test_speech.data()), std::streamsize(bytes));
    }
  }
  engine->set_test_devices(
      [](float* out, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
          out[i] = test_speech.empty() ? 0.0f : test_speech[test_position % test_speech.size()];
          ++test_position;
        }
      },
      [](const float* stereo, std::size_t count) {
        double energy = 0.0;
        for (std::size_t i = 0; i < count * 2; ++i) {
          energy += double(stereo[i]) * double(stereo[i]);
        }
        test_heard_energy.store(test_heard_energy.load() + energy);
        test_heard_samples.fetch_add(count * 2);
        if (test_listen_file) {
          std::fwrite(stereo, sizeof(float), count * 2, test_listen_file);
        }
      });
  if (const char* listen_file = std::getenv("KCDS_TEST_VOICE_LISTEN_FILE")) {
    test_listen_file = std::fopen(listen_file, "wb");
    log("VOICE test listen file start_ms=%llu", static_cast<unsigned long long>(GetTickCount64()));
  }
  test_running.store(true);
  test_thread = std::thread(test_thread_main);
  log("VOICE test devices: speech %zu samples start_ms=%llu", test_speech.size(),
      static_cast<unsigned long long>(GetTickCount64()));
}

void log_stats_now_and_then() {
  static std::uint64_t logged_at = 0;
  const std::uint64_t now = GetTickCount64();
  const std::uint64_t every = test_running.load() ? 2000 : 60000;
  if (now - logged_at < every) {
    return;
  }
  logged_at = now;
  std::lock_guard<std::mutex> lock(engine_mutex);
  if (!engine || !engine->active()) {
    return;
  }
  const EngineStats stats = engine->stats();
  const std::uint64_t samples = test_heard_samples.exchange(0);
  const double energy = test_heard_energy.exchange(0.0);
  log("VOICE stats mic=%d out=%d hrtf=%d talking=%d sent=%llu got=%llu concealed=%llu late=%llu "
      "streams=%u level=%.3f heard_rms=%.4f",
      int(stats.has_microphone), int(stats.has_output), int(stats.hrtf), int(stats.transmitting),
      static_cast<unsigned long long>(stats.frames_sent),
      static_cast<unsigned long long>(stats.frames_received),
      static_cast<unsigned long long>(stats.frames_concealed),
      static_cast<unsigned long long>(stats.frames_late), stats.streams, stats.input_level,
      samples ? std::sqrt(energy / double(samples)) : 0.0);
}

// Under engine_mutex.
void make_engine() {
  if (engine) {
    return;
  }
  engine = std::make_unique<VoiceEngine>([](const std::vector<std::uint8_t>& data) {
    if (const DatagramSender sender = send_datagram.load()) {
      sender(data.data(), data.size());
    }
  });
  start_test_devices_if_asked();
}

void take_state() {
  std::optional<integration::VoiceState> told;
  {
    using kcds::game_client::client;
    using kcds::game_client::network_mutex;
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      told = client->peer().take_voice_state();
      for (const auto& [talker, amount] : client->peer().take_voice_muffles()) {
        if (amount == 0) {
          muffled.erase(talker);
        } else {
          muffled[talker] = float(amount) / 255.0f;
        }
      }
    }
  }
  if (!told) {
    return;
  }
  state = *told;
  have_state = true;
  Settings values = settings();
  // The network thread takes the network lock, then the engine lock (pump, on_datagram): this never
  // holds the engine lock while it takes the network lock.
  if (!state.enabled) {
    std::lock_guard<std::mutex> lock(engine_mutex);
    make_engine();
    log("VOICE off by the server");
    engine->set_active(false);
    engine->clear_session();
    keyed_session = 0;
    return;
  }
  const bool new_session = keyed_session != state.session || keyed_server != state.server_key;
  std::array<std::uint8_t, 32> key_bytes{};
  if (new_session) {
    own_keys = kcd2s::voice::make_key_pair();
    kcd2s::voice::Key server{};
    std::copy(state.server_key.begin(), state.server_key.end(), server.begin());
    kcd2s::voice::Key session_key{};
    if (!kcd2s::voice::derive_session_key(own_keys.secret_key, server, own_keys.public_key, server,
                                          session_key)) {
      log("VOICE the server's key is refused");
      return;
    }
    std::copy(session_key.begin(), session_key.end(), key_bytes.begin());
    std::array<std::uint8_t, 32> own_public{};
    std::copy(own_keys.public_key.begin(), own_keys.public_key.end(), own_public.begin());
    using kcds::game_client::client;
    using kcds::game_client::network_mutex;
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!client) {
      return;
    }
    client->set_voice_endpoint(state.voice_ip, state.voice_port);
    if (!client->peer().send_voice_key(own_public)) {
      return;
    }
  }
  std::lock_guard<std::mutex> lock(engine_mutex);
  make_engine();
  if (new_session) {
    engine->set_active(true);
    apply_settings(values);
    engine->set_session(state.session, key_bytes);
    keyed_session = state.session;
    keyed_server = state.server_key;
    log("VOICE on: session %u", state.session);
  }
  engine->set_tier(state.tier, state.ranges[state.tier]);
  engine->set_muted(state.muted);
  engine->set_deaf(state.deaf);
}

void update_positions() {
  using kcds::game_client::remotes;
  using kcds::game_client::view_matrix;
  using kcds::game_client::view_matrix_valid;
  std::lock_guard<std::mutex> lock(engine_mutex);
  if (!engine || !engine->active()) {
    return;
  }
  if (view_matrix_valid) {
    const auto& m = view_matrix;
    engine->set_listener(kcds::voice::Vec3{m[3], m[7], m[11]}, kcds::voice::Vec3{m[1], m[5], m[9]},
                         kcds::voice::Vec3{m[2], m[6], m[10]});
  }
  for (const std::uint32_t talker : engine->talkers()) {
    const auto found = remotes.find(talker);
    if (found == remotes.end()) {
      continue;
    }
    void* body = kcds::runtime::props::entity::by_id(found->second.engine_id);
    if (!body) {
      continue;
    }
    float position[3];
    float rotation[4];
    kcds::runtime::props::entity::world_pose(body, position, rotation);
    // At the head: the body's place is at its feet.
    engine->set_talker_position(talker,
                                kcds::voice::Vec3{position[0], position[1], position[2] + 1.65f});
  }
}

// Each 200 ms: what is between each talker and the listener, and the room the listener is in.
// Physics on the engine thread, at most 8 rays, 5 times a second.
struct Zone {
  float min[3];
  float max[3];
  kcds::voice::RoomPreset room;
  float occlusion;
};
std::mutex zones_mutex;
std::vector<Zone> zones;

bool inside(const Zone& zone, const float* point) {
  for (int axis = 0; axis < 3; ++axis) {
    if (point[axis] < zone.min[axis] || point[axis] > zone.max[axis]) {
      return false;
    }
  }
  return true;
}

// What the talker's replica says of them: its owner's reports, and its soul for a knock-out.
// The local player's own visor (set_local_visor_closed), and how much it muffles everyone.
std::atomic<bool> local_visor_closed{false};
constexpr float kOwnVisorMuffle = 0.45f;

kcds::voice::TalkerState state_of(const kcds::game_client::Remote& remote) {
  kcds::voice::TalkerState talker;
  talker.visor_closed = remote.voice_visor_closed;
  talker.riding = remote.voice_riding;
  std::vector<integration::ReplicaBuff> buffs;
  if (integration::parse_replica_buffs_field(remote.replica_buffs_applied, buffs)) {
    for (const integration::ReplicaBuff& buff : buffs) {
      if (buff.amount) {
        talker.drunkenness = std::max(talker.drunkenness, float(*buff.amount) / 1000.0f);
      }
      if (buff.injury) {
        talker.injury =
            std::max(talker.injury, float(buff.injury->severity) / integration::kInjurySeverityOne);
      }
    }
  }
  integration::PlayerVitals vitals;
  integration::PlayerAbilities abilities;
  if (integration::parse_vitals_field(remote.voice_vitals, vitals) &&
      integration::parse_abilities_field(remote.voice_abilities, abilities) &&
      abilities.maxima[1] > 0.0f) {
    talker.stamina = vitals.values[1] / abilities.maxima[1];
  }
  if (remote.actor && kcds::game_client::native_soul_is_unconscious) {
    void* soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(remote.actor) + 1640);
    talker.unconscious = soul && kcds::game_client::native_soul_is_unconscious(soul);
  }
  return talker;
}

void update_effects() {
  using kcds::game_client::remotes;
  using kcds::game_client::view_matrix;
  using kcds::game_client::view_matrix_valid;
  static std::uint64_t updated_at = 0;
  const std::uint64_t now = GetTickCount64();
  if (now - updated_at < 200 || !view_matrix_valid) {
    return;
  }
  updated_at = now;
  const float listener[3] = {view_matrix[3], view_matrix[7], view_matrix[11]};
  // A zone's room wins; without one, the game knows only inside or outside: its own "interior"
  // context on the player (houses), or a vis area (world/interior.hpp).
  const bool interior = kcds::runtime::world::interior::player_inside();
  const bool vis_area = kcds::runtime::world::point_indoors(listener);
  kcds::voice::RoomPreset room = kcds::voice::RoomPreset::Outdoors;
  if (interior || vis_area) {
    room = kcds::voice::RoomPreset::Tavern;
  }
  static int logged_inside = -1;
  const int inside_now = (interior ? 1 : 0) | (vis_area ? 2 : 0);
  if (inside_now != logged_inside) {
    logged_inside = inside_now;
    log("VOICE listener interior=%u vis_area=%u", unsigned(interior), unsigned(vis_area));
  }
  std::vector<Zone> places;
  {
    std::lock_guard<std::mutex> lock(zones_mutex);
    places = zones;
  }
  float listener_zone_occlusion = 0.0f;
  for (const Zone& zone : places) {
    if (inside(zone, listener)) {
      room = zone.room;
      listener_zone_occlusion = zone.occlusion;
      break;
    }
  }
  const float rain = kcds::runtime::world::rain_intensity();
  std::vector<std::uint32_t> talkers;
  {
    std::lock_guard<std::mutex> lock(engine_mutex);
    if (!engine || !engine->active()) {
      return;
    }
    engine->set_room(room);
    talkers = engine->talkers();
  }
  for (const std::uint32_t talker : talkers) {
    const auto found = remotes.find(talker);
    if (found == remotes.end()) {
      continue;
    }
    void* body = kcds::runtime::props::entity::by_id(found->second.engine_id);
    if (!body) {
      continue;
    }
    float position[3];
    float rotation[4];
    kcds::runtime::props::entity::world_pose(body, position, rotation);
    position[2] += 1.65f;
    kcds::voice::TalkerEffect effect;
    const bool wall = !kcds::runtime::physics::clear_between(listener, position);
    if (wall) {
      effect.occlusion = 1.0f;
    }
    float talker_zone_occlusion = 0.0f;
    for (const Zone& zone : places) {
      if (inside(zone, position)) {
        talker_zone_occlusion = zone.occlusion;
        break;
      }
    }
    // In different zones, the larger of the two walls counts.
    effect.occlusion =
        std::max(effect.occlusion, std::max(listener_zone_occlusion, talker_zone_occlusion));
    // Outdoors, heavy rain covers a voice some.
    if (room == kcds::voice::RoomPreset::Outdoors) {
      effect.masking = rain * 0.5f;
    }
    kcds::voice::apply_talker_state(state_of(found->second), effect);
    if (local_visor_closed.load()) {
      effect.muffle = std::max(effect.muffle, kOwnVisorMuffle);
    }
    const auto gag = muffled.find(talker);
    if (gag != muffled.end()) {
      effect.muffle = std::max(effect.muffle, gag->second);
    }
    // What changes is logged, for the live tests of.
    static std::map<std::uint32_t, std::array<float, 3>> logged;
    const std::array<float, 3> seen{wall ? 1.0f : 0.0f, effect.occlusion, effect.muffle};
    const auto known = logged.find(talker);
    if (known == logged.end() || known->second != seen) {
      logged[talker] = seen;
      log("VOICE talker %u wall=%u occlusion=%.2f muffle=%.2f room=%d from %.1f %.1f %.1f to %.1f "
          "%.1f %.1f",
          talker, unsigned(wall), effect.occlusion, effect.muffle, int(room), listener[0],
          listener[1], listener[2], position[0], position[1], position[2]);
    }
    std::lock_guard<std::mutex> lock(engine_mutex);
    if (engine) {
      engine->set_talker_effect(talker, effect);
    }
  }
}

void update_keys() {
  const bool hooked_test = std::getenv("KCDS_TEST_VOICE_FILE") != nullptr;
  const bool in_front = game_in_front();
  const bool talk = hooked_test || (in_front && action_held(kTalkAction));
  const bool tier_key = in_front && action_held(kTierAction);
  {
    std::lock_guard<std::mutex> lock(engine_mutex);
    if (engine) {
      engine->set_push_to_talk(talk);
    }
  }
  if (tier_key && !tier_key_was_down && have_state && state.enabled) {
    const auto next = std::uint8_t((state.tier + 1) % integration::kVoiceTierCount);
    using kcds::game_client::client;
    using kcds::game_client::network_mutex;
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      client->peer().send_voice_tier(next);
    }
  }
  tier_key_was_down = tier_key;
}

void show_on_hud() {
  static const char* const kTierNames[] = {"Whisper", "Normal", "Shout"};
  bool talking = false;
  {
    std::lock_guard<std::mutex> lock(engine_mutex);
    talking = engine && engine->active() && engine->stats().transmitting;
  }
  if (talking != was_talking) {
    was_talking = talking;
    log("VOICE %s (%s)", talking ? "transmitting" : "silent",
        have_state ? kTierNames[state.tier] : "");
  }
  // The voice indicator, unless the server shows its own (Voice.setIndicator). The
  // "own microphone" setting off: it does not light up while the player is heard.
  indicator::State shown;
  shown.shown = have_state && state.enabled && state.indicator;
  shown.tier = state.tier;
  shown.muted = state.muted;
  shown.talking = talking && settings().show_own_mic;
  shown.push_to_talk = !settings().voice_activation;
  shown.key = push_to_talk_key();
  shown.level = (std::min)(1.0f, std::sqrt(input_level()) * 2.0f);
  indicator::set(shown);
  {
    std::lock_guard<std::mutex> lock(engine_mutex);
    heard_now.clear();
    if (engine && engine->active()) {
      heard_now = engine->talkers();
    }
  }
}

}  // namespace

bool talking_icons_shown() {
  return settings().show_talking_icons;
}

float input_level() {
  std::lock_guard<std::mutex> lock(engine_mutex);
  return engine && engine->active() ? engine->stats().input_level : 0.0f;
}

bool is_talking(std::uint32_t player) {
  return std::find(heard_now.begin(), heard_now.end(), player) != heard_now.end();
}

void set_local_visor_closed(bool closed) {
  local_visor_closed.store(closed);
}

void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send) {
  static bool told_any = false;
  static std::string told;
  bool transmitting = false;
  bool microphone = false;
  {
    std::lock_guard<std::mutex> lock(engine_mutex);
    if (engine && engine->active()) {
      const EngineStats stats = engine->stats();
      transmitting = stats.transmitting;
      microphone = stats.has_microphone;
    }
  }
  const bool voice_mode = settings().voice_activation;
  const bool enabled = have_state && state.enabled;
  std::ostringstream key;
  key << enabled << int(state.tier) << state.muted << state.deaf << transmitting << microphone
      << voice_mode;
  if (told_any && told == key.str()) {
    return;
  }
  using kcd2s::sdk::Value;
  Value::Map message{{"op", Value(std::string("voiceState"))},
                     {"enabled", Value(enabled)},
                     {"tier", Value(std::int64_t(state.tier))},
                     {"muted", Value(state.muted)},
                     {"deaf", Value(state.deaf)},
                     {"transmitting", Value(transmitting)},
                     {"hasMicrophone", Value(microphone)},
                     {"mode", Value(std::string(voice_mode ? "voice" : "push"))}};
  if (send(Value(std::move(message)))) {
    told = key.str();
    told_any = true;
  }
}

Settings settings() {
  std::lock_guard<std::mutex> lock(settings_mutex);
  read_settings_once();
  return current;
}

std::vector<std::string> input_devices() {
  std::lock_guard<std::mutex> lock(engine_mutex);
  make_engine();
  return engine->input_devices();
}

std::vector<std::string> output_devices() {
  std::lock_guard<std::mutex> lock(engine_mutex);
  make_engine();
  return engine->output_devices();
}

void set_settings(const Settings& changed) {
  {
    std::lock_guard<std::mutex> lock(settings_mutex);
    settings_read = true;
    current = changed;
    write_settings(current);
  }
  std::lock_guard<std::mutex> lock(engine_mutex);
  apply_settings(changed);
}

void session_started(DatagramSender sender) {
  send_datagram.store(sender);
  namespace keybinds = kcds::runtime::keybinds;
  keybinds::add_builtin_group(keybinds::kKcdsGroup, keybinds::kKcdsGroupLabel);
  keybinds::add_builtin_action(kTalkAction, keybinds::kKcdsGroup, "Talk (push to talk)", "mouse4");
  keybinds::add_builtin_action(kTierAction, keybinds::kKcdsGroup, "Whisper, normal or shout", "f9");
}

void session_ended() {
  std::lock_guard<std::mutex> lock(engine_mutex);
  if (engine) {
    engine->set_active(false);
    engine->clear_session();
  }
  send_datagram.store(nullptr);
  keyed_session = 0;
  have_state = false;
}

void on_datagram(const std::uint8_t* data, std::size_t size) {
  std::lock_guard<std::mutex> lock(engine_mutex);
  if (engine) {
    engine->on_datagram(data, size);
  }
}

void say_hello_now_and_then() {
  static std::uint64_t said_at = 0;
  const std::uint64_t now = GetTickCount64();
  if (now - said_at < 1000) {
    return;
  }
  said_at = now;
  std::lock_guard<std::mutex> lock(engine_mutex);
  if (engine && engine->active() && keyed_session != 0) {
    engine->say_hello();
  }
}

void frame() {
  if (!send_datagram.load()) {
    return;
  }
  take_state();
  say_hello_now_and_then();
  update_keys();
  update_positions();
  update_effects();
  show_on_hud();
  log_stats_now_and_then();
}

bool handle_runtime_message(const kcd2s::sdk::Value& message) {
  const auto* op = kcd2s::integration::script_field(message, "op");
  if (!op || op->tag() != kcd2s::sdk::Tag::Str || op->as_str().rfind("voice", 0) != 0) {
    return false;
  }
  if (op->as_str() == "voiceTierAsk") {
    const auto* tier = kcd2s::integration::script_field(message, "tier");
    const std::int64_t asked = !tier                                 ? -1
                               : tier->tag() == kcd2s::sdk::Tag::Int ? tier->as_int()
                               : tier->tag() == kcd2s::sdk::Tag::Double
                                   ? std::int64_t(tier->as_double())
                                   : -1;
    using kcds::game_client::client;
    if (client && asked >= 0 && asked < integration::kVoiceTierCount) {
      // The network thread, with the network lock held: this message came through it.
      client->peer().send_voice_tier(std::uint8_t(asked));
    }
    return true;
  }
  if (op->as_str() == "voiceZones") {
    std::vector<Zone> made;
    const auto* list = kcd2s::integration::script_field(message, "zones");
    if (list && list->tag() == kcd2s::sdk::Tag::Array) {
      const auto& numbers = list->as_array();
      const auto number = [&](std::size_t index) {
        const auto& value = numbers[index];
        return value.tag() == kcd2s::sdk::Tag::Int      ? float(value.as_int())
               : value.tag() == kcd2s::sdk::Tag::Double ? float(value.as_double())
                                                        : 0.0f;
      };
      for (std::size_t at = 0; at + 8 <= numbers.size() && made.size() < 512; at += 8) {
        Zone zone{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
          zone.min[axis] = number(at + axis);
          zone.max[axis] = number(at + 3 + axis);
        }
        const int room = int(number(at + 6));
        zone.room = room >= 0 && room <= 5 ? static_cast<kcds::voice::RoomPreset>(room)
                                           : kcds::voice::RoomPreset::Outdoors;
        zone.occlusion = std::clamp(number(at + 7), 0.0f, 1.0f);
        made.push_back(zone);
      }
    }
    std::lock_guard<std::mutex> lock(zones_mutex);
    zones = std::move(made);
    return true;
  }
  if (op->as_str() == "voiceSettings") {
    Settings values = settings();
    if (const auto* mode = kcd2s::integration::script_field(message, "mode")) {
      if (mode->tag() == kcd2s::sdk::Tag::Str) {
        values.voice_activation = mode->as_str() == "voice";
      }
    }
    set_settings(values);
    return true;
  }
  return false;
}

}  // namespace kcds::runtime::voice
