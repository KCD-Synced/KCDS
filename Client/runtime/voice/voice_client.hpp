// Voice chat in the game client: joins the voice engine (Client/voice) to the
// session. The engine does its work on its own threads; these functions are quick and never wait
// for audio.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::voice {

// How the player talks; kept on this computer in %LOCALAPPDATA%/KCDS/voice.json.
struct Settings {
  bool voice_activation = false;
  float input_volume = 1.0f;
  float output_volume = 1.0f;
  std::string input_device;
  std::string output_device;
  // Voice activation: how sure it must be that this is speech, 0.3 (opens easily) to 0.9.
  float voice_threshold = 0.6f;
  // A microphone beside the names of those talking, and beside the player's own voice line.
  bool show_talking_icons = true;
  bool show_own_mic = true;
  // For players on speakers: their voices taken back out of the microphone.
  bool echo_cancellation = false;
};

// Before a session's first frame: where its voice datagrams go.
using DatagramSender = bool (*)(const std::uint8_t* data, std::size_t size);
void session_started(DatagramSender sender);
// The session ends: every device closes, before the socket goes.
void session_ended();
// Whether the player wants the microphone beside the names of those talking.
bool talking_icons_shown();
// How loud the microphone is now, 0 to 1, for the sound page's microphone test.
float input_level();

// The network thread: a voice datagram from the server.
void on_datagram(const std::uint8_t* data, std::size_t size);

// Engine thread, every frame.
void frame();

// Network thread, after each tick: tells the resource runtime the state when it changed.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

// For local::Client::set_local_runtime_handler: true when the message was a voice one.
//   { op: "voiceSettings", ...any of Settings }, { op: "voiceTierAsk", tier }
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Whether this player's voice is being heard now, for their plate. Engine thread.
bool is_talking(std::uint32_t player);

// Whether the local player's own helmet visor is down: everyone sounds muffled to them then.
void set_local_visor_closed(bool closed);

Settings settings();
void set_settings(const Settings& changed);
// The microphones and the outputs this computer has, by name.
std::vector<std::string> input_devices();
std::vector<std::string> output_devices();

}  // namespace kcds::runtime::voice
