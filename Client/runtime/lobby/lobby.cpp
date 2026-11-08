#include "lobby.hpp"

#include "../core/log.hpp"

#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <chrono>
#include <mutex>

namespace kcds::runtime::lobby {
namespace {

std::atomic<bool> held_now{false};
std::mutex sound_mutex;

// Releases a COM interface when it goes out of scope.
template <typename Interface>
struct Held {
  Interface* pointer = nullptr;
  ~Held() {
    if (pointer) {
      pointer->Release();
    }
  }
  Interface** out() {
    return &pointer;
  }
};

// Mutes or unmutes every audio session of this process on the default output. The number of
// sessions changed, or -1 when Windows' audio could not be asked.
int mute_own_sessions(bool mute) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool own_com = SUCCEEDED(com);
  int changed = -1;
  do {
    Held<IMMDeviceEnumerator> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(devices.out())))) {
      break;
    }
    Held<IMMDevice> output;
    if (FAILED(devices.pointer->GetDefaultAudioEndpoint(eRender, eConsole, output.out()))) {
      break;
    }
    Held<IAudioSessionManager2> manager;
    if (FAILED(output.pointer->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(manager.out())))) {
      break;
    }
    Held<IAudioSessionEnumerator> sessions;
    if (FAILED(manager.pointer->GetSessionEnumerator(sessions.out()))) {
      break;
    }
    int count = 0;
    if (FAILED(sessions.pointer->GetCount(&count))) {
      break;
    }
    changed = 0;
    const DWORD self = GetCurrentProcessId();
    for (int index = 0; index < count; ++index) {
      Held<IAudioSessionControl> control;
      if (FAILED(sessions.pointer->GetSession(index, control.out()))) {
        continue;
      }
      Held<IAudioSessionControl2> details;
      if (FAILED(control.pointer->QueryInterface(__uuidof(IAudioSessionControl2),
                                                 reinterpret_cast<void**>(details.out())))) {
        continue;
      }
      DWORD process = 0;
      if (FAILED(details.pointer->GetProcessId(&process)) || process != self) {
        continue;
      }
      Held<ISimpleAudioVolume> volume;
      if (FAILED(control.pointer->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                 reinterpret_cast<void**>(volume.out())))) {
        continue;
      }
      if (SUCCEEDED(volume.pointer->SetMute(mute ? TRUE : FALSE, nullptr))) {
        changed += 1;
      }
    }
  } while (false);
  if (own_com) {
    CoUninitialize();
  }
  return changed;
}

}  // namespace

bool set(bool held) {
  if (held_now.exchange(held) == held) {
    return false;
  }
  std::lock_guard<std::mutex> lock(sound_mutex);
  const int sessions = mute_own_sessions(held);
  kcds::game_client::log("LOBBY %s (sound sessions %s: %d)",
                         held ? "held out of the world" : "let in", held ? "muted" : "unmuted",
                         sessions);
  return true;
}

bool held() {
  return held_now.load();
}

void keep_muted() {
  constexpr auto kEvery = std::chrono::seconds(1);
  static std::chrono::steady_clock::time_point next;
  if (!held_now.load()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(sound_mutex);
  if (now < next) {
    return;
  }
  next = now + kEvery;
  mute_own_sessions(true);
}

}  // namespace kcds::runtime::lobby
