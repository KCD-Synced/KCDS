#include "voice_settings_page.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../native_ui/native_ui.hpp"
#include "MinHook.h"
#include "kcd2s/sdk/value.hpp"
#include "voice_client.hpp"

namespace kcds::runtime::voice::settings_page {
namespace {

namespace sdk = kcd2s::sdk;

using SoundPage = std::int64_t(__fastcall*)(void* settings, void* page);
using ReadVolumes = float*(__fastcall*)(void* settings, float* out);
using ValueEvent = std::int64_t(__fastcall*)(void* settings, void* id, float value);
using ChoiceEvent = std::int64_t(__fastcall*)(void* settings, void* id, unsigned choice);
using ApplyVolumes = double(__fastcall*)(void* settings, const float* volumes, char refresh);

SoundPage original_sound_page = nullptr;
ReadVolumes original_read_volumes = nullptr;
ValueEvent original_value_event = nullptr;
ChoiceEvent original_choice_event = nullptr;
ApplyVolumes original_apply_volumes = nullptr;
Logger log_line = nullptr;

// The list container of the page's rows (menu_buttons.xml: rows without a Container are in 0).
constexpr std::int64_t kListContainer = 0;
constexpr const char* kMenuElement = "Menu";

// The rows' ids: the game's button names know none of them.
constexpr const char* kVolume = "KcdsVoiceVolume";
constexpr const char* kMicVolume = "KcdsVoiceMicVolume";
constexpr const char* kMicTest = "KcdsVoiceMicTest";
constexpr const char* kMode = "KcdsVoiceMode";
constexpr const char* kSensitivity = "KcdsVoiceSensitivity";
constexpr const char* kInput = "KcdsVoiceInput";
constexpr const char* kOutput = "KcdsVoiceOutput";
constexpr const char* kEcho = "KcdsVoiceEcho";
constexpr const char* kTalkingIcons = "KcdsVoiceTalkingIcons";
constexpr const char* kOwnMic = "KcdsVoiceOwnMic";

// Voice activation's threshold (how sure it must be that this is speech) runs from 0.9 (the
// slider's left, hardest to open) to 0.3 (its right, opens easily).
constexpr float kThresholdHardest = 0.9f;
constexpr float kThresholdEasiest = 0.3f;

// Raised on the game thread while the game builds its sound page.
thread_local bool building_sound_page = false;
thread_local bool rows_added = false;

// C_UISettings keeps the volumes in effect at +230h and the defaults at +118h. Leaving the page
// with No (or with nothing changed) applies the ones in effect again, Reset the defaults, Confirm
// the sliders'.
constexpr std::size_t kVolumesInEffect = 0x230;
constexpr std::size_t kVolumeDefaults = 0x118;

// The voice settings when the page was built: the rows change them at once, so that the player
// hears them; leaving with No puts these back.
bool page_open = false;
Settings settings_on_open;

// The device lists the choices were built from: a choice's id is its place here plus one (0 is
// the computer's default).
std::vector<std::string> shown_inputs;
std::vector<std::string> shown_outputs;

sdk::Value call(const char* function, sdk::Value::Array args) {
  return kcds::runtime::native_ui::request(
      sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("elementCall"))},
                                 {"name", sdk::Value(std::string(kMenuElement))},
                                 {"instance", sdk::Value(std::int64_t{0})},
                                 {"fn", sdk::Value(std::string(function))},
                                 {"args", sdk::Value(std::move(args))}}));
}

void add_slider(const char* id, const char* label, float value, float least, float most,
                const char* tooltip, bool disabled = false) {
  call("AddValueButton",
       sdk::Value::Array{sdk::Value(std::string(id)), sdk::Value(kListContainer),
                         sdk::Value(std::string(label)), sdk::Value(double(value)),
                         sdk::Value(double(least)), sdk::Value(double(most)),
                         sdk::Value(std::string(tooltip)), sdk::Value(disabled)});
}

void add_choice(const char* id, const char* label, const std::vector<std::string>& options,
                int chosen, const char* tooltip) {
  call("AddChoicesButton",
       sdk::Value::Array{sdk::Value(std::string(id)), sdk::Value(kListContainer),
                         sdk::Value(std::string(label)), sdk::Value(std::string(tooltip)),
                         sdk::Value(false)});
  for (std::size_t index = 0; index < options.size(); ++index) {
    call("AddChoiceOption",
         sdk::Value::Array{sdk::Value(std::int64_t(index)), sdk::Value(std::string(id)),
                           sdk::Value(kListContainer), sdk::Value(options[index]),
                           sdk::Value(std::string()), sdk::Value(false)});
  }
  call("SetChoice", sdk::Value::Array{sdk::Value(std::string(id)), sdk::Value(kListContainer),
                                      sdk::Value(std::int64_t(chosen))});
}

void add_switch(const char* id, const char* label, bool on, const char* tooltip) {
  add_choice(id, label, {"Off", "On"}, on ? 1 : 0, tooltip);
}

// The device choice: the computer's default first, then each device by name.
int device_choice(const std::vector<std::string>& devices, const std::string& chosen) {
  for (std::size_t index = 0; index < devices.size(); ++index) {
    if (devices[index] == chosen) {
      return int(index) + 1;
    }
  }
  return 0;
}

std::vector<std::string> device_options(const std::vector<std::string>& devices) {
  std::vector<std::string> options{"Default"};
  options.insert(options.end(), devices.begin(), devices.end());
  return options;
}

void add_rows() {
  const Settings values = kcds::runtime::voice::settings();
  shown_inputs = kcds::runtime::voice::input_devices();
  shown_outputs = kcds::runtime::voice::output_devices();
  const float sensitivity =
      (kThresholdHardest - values.voice_threshold) / (kThresholdHardest - kThresholdEasiest);
  add_slider(kVolume, "Voice chat volume", values.output_volume, 0.0f, 2.0f,
             "How loud the other players' voices are.");
  add_slider(kMicVolume, "Microphone volume", values.input_volume, 0.0f, 4.0f,
             "How loud your voice is for the others.");
  // frame() sets it to the microphone's level (a disabled slider shows no value).
  add_slider(kMicTest, "Microphone test", 0.0f, 0.0f, 1.0f, "Speak: the bar moves.");
  add_choice(kMode, "Voice chat talk mode", {"Push to talk", "By voice"},
             values.voice_activation ? 1 : 0,
             "Push to talk sends your voice while you hold the talk key (Key bindings).");
  add_slider(kSensitivity, "Voice activation sensitivity", sensitivity, 0.0f, 1.0f,
             "Higher opens the microphone for quieter speech.");
  add_choice(kInput, "Microphone", device_options(shown_inputs),
             device_choice(shown_inputs, values.input_device), "");
  add_choice(kOutput, "Voice chat output", device_options(shown_outputs),
             device_choice(shown_outputs, values.output_device), "");
  add_switch(kEcho, "Echo cancellation", values.echo_cancellation,
             "For speakers: the others' voices are taken back out of your microphone.");
  add_switch(kTalkingIcons, "Talking icons", values.show_talking_icons,
             "A microphone beside the names of the players talking.");
  add_switch(kOwnMic, "Own microphone icon", values.show_own_mic,
             "Your voice indicator lights up while you are heard.");
  if (log_line) {
    log_line("VOICE settings rows added to the sound page (%zu inputs, %zu outputs)",
             shown_inputs.size(), shown_outputs.size());
  }
}

std::int64_t __fastcall hooked_sound_page(void* settings, void* page) {
  building_sound_page = true;
  rows_added = false;
  page_open = true;
  settings_on_open = kcds::runtime::voice::settings();
  const std::int64_t result = original_sound_page(settings, page);
  building_sound_page = false;
  return result;
}

float* __fastcall hooked_read_volumes(void* settings, float* out) {
  float* result = original_read_volumes(settings, out);
  if (building_sound_page && !rows_added) {
    rows_added = true;
    add_rows();
  }
  return result;
}

const char* id_text(void* id) {
  const char* const* text = static_cast<const char* const*>(id);
  return text && *text ? *text : "";
}

std::string device_of(const std::vector<std::string>& devices, unsigned choice) {
  if (choice == 0 || choice > devices.size()) {
    return std::string();
  }
  return devices[choice - 1];
}

std::int64_t __fastcall hooked_value_event(void* settings, void* id, float value) {
  const std::string name = id_text(id);
  Settings values = kcds::runtime::voice::settings();
  bool ours = true;
  if (name == kVolume) {
    values.output_volume = value;
  } else if (name == kMicVolume) {
    values.input_volume = value;
  } else if (name == kSensitivity) {
    values.voice_threshold = kThresholdHardest - value * (kThresholdHardest - kThresholdEasiest);
  } else {
    ours = false;
  }
  if (ours) {
    kcds::runtime::voice::set_settings(values);
  }
  // The game's handler ignores an id of ours, and frees the id.
  return original_value_event(settings, id, value);
}

std::int64_t __fastcall hooked_choice_event(void* settings, void* id, unsigned choice) {
  const std::string name = id_text(id);
  Settings values = kcds::runtime::voice::settings();
  bool ours = true;
  if (name == kMode) {
    values.voice_activation = choice == 1;
  } else if (name == kInput) {
    values.input_device = device_of(shown_inputs, choice);
  } else if (name == kOutput) {
    values.output_device = device_of(shown_outputs, choice);
  } else if (name == kEcho) {
    values.echo_cancellation = choice == 1;
  } else if (name == kTalkingIcons) {
    values.show_talking_icons = choice == 1;
  } else if (name == kOwnMic) {
    values.show_own_mic = choice == 1;
  } else {
    ours = false;
  }
  if (ours) {
    kcds::runtime::voice::set_settings(values);
    if (log_line) {
      log_line("VOICE settings %s -> choice %u", name.c_str(), choice);
    }
  }
  return original_choice_event(settings, id, choice);
}

double __fastcall hooked_apply_volumes(void* settings, const float* volumes, char refresh) {
  const auto* base = static_cast<const std::uint8_t*>(settings);
  if (page_open) {
    if (reinterpret_cast<const std::uint8_t*>(volumes) == base + kVolumesInEffect) {
      // No, or Back with nothing changed: the voice settings as they were.
      kcds::runtime::voice::set_settings(settings_on_open);
      page_open = false;
    } else if (reinterpret_cast<const std::uint8_t*>(volumes) == base + kVolumeDefaults) {
      // Reset: the defaults, which the rebuilt page then shows.
      kcds::runtime::voice::set_settings(Settings{});
    } else {
      // Confirm: what the rows set stays.
      page_open = false;
    }
  }
  return original_apply_volumes(settings, volumes, refresh);
}

// The microphone's level on the meter: the square root shows quiet speech, as an ear hears it.
float meter_of(float level) {
  // In parentheses: Windows.h (through MinHook.h) defines a min macro.
  return (std::min)(1.0f, std::sqrt(level) * 2.0f);
}

bool hook(void* target, void* detour, void** original) {
  return MH_CreateHook(target, detour, original) == MH_OK;
}

}  // namespace

void frame() {
  static auto shown_at = std::chrono::steady_clock::time_point{};
  if (!page_open) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - shown_at < std::chrono::milliseconds(100)) {
    return;
  }
  shown_at = now;
  const float meter = meter_of(kcds::runtime::voice::input_level());
  call("SetValue", sdk::Value::Array{sdk::Value(std::string(kMicTest)), sdk::Value(kListContainer),
                                     sdk::Value(double(meter))});
}

bool install(const Natives& natives) {
  log_line = natives.log;
  if (!natives.sound_page || !natives.read_volumes || !natives.value_event ||
      !natives.choice_event || !natives.apply_volumes) {
    return false;
  }
  const bool created = hook(natives.sound_page, reinterpret_cast<void*>(hooked_sound_page),
                            reinterpret_cast<void**>(&original_sound_page)) &&
                       hook(natives.read_volumes, reinterpret_cast<void*>(hooked_read_volumes),
                            reinterpret_cast<void**>(&original_read_volumes)) &&
                       hook(natives.value_event, reinterpret_cast<void*>(hooked_value_event),
                            reinterpret_cast<void**>(&original_value_event)) &&
                       hook(natives.choice_event, reinterpret_cast<void*>(hooked_choice_event),
                            reinterpret_cast<void**>(&original_choice_event)) &&
                       hook(natives.apply_volumes, reinterpret_cast<void*>(hooked_apply_volumes),
                            reinterpret_cast<void**>(&original_apply_volumes));
  if (!created) {
    MH_RemoveHook(natives.sound_page);
    MH_RemoveHook(natives.read_volumes);
    MH_RemoveHook(natives.value_event);
    MH_RemoveHook(natives.choice_event);
    MH_RemoveHook(natives.apply_volumes);
    return false;
  }
  MH_EnableHook(natives.sound_page);
  MH_EnableHook(natives.read_volumes);
  MH_EnableHook(natives.value_event);
  MH_EnableHook(natives.choice_event);
  MH_EnableHook(natives.apply_volumes);
  return true;
}

}  // namespace kcds::runtime::voice::settings_page
