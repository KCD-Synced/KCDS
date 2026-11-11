#include "web_audio.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "web_audio_level.hpp"

namespace kcds::runtime::web::audio {
namespace {

constexpr wchar_t kHelper[] = L"kcds-web-helper.exe";
constexpr auto kApplyEvery = std::chrono::seconds(1);

Logger log_line = nullptr;
std::once_flag started;
std::mutex state_mutex;
std::condition_variable changed;
bool setting_read = false;
float player_setting = 1.0f;
float server_volume = 100.0f;
bool server_muted = false;
bool dirty = true;

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

std::string setting_path() {
  const char* base = std::getenv("LOCALAPPDATA");
  return std::string(base ? base : ".") + "\\KCDS\\web-audio.json";
}

// Under state_mutex.
void read_setting_once() {
  if (setting_read) {
    return;
  }
  setting_read = true;
  std::ifstream file(setting_path());
  if (!file) {
    return;
  }
  std::stringstream text;
  text << file.rdbuf();
  const std::string all = text.str();
  const std::string key = "\"volume\": ";
  const auto at = all.find(key);
  if (at != std::string::npos) {
    player_setting = clamped(float(std::atof(all.c_str() + at + key.size())), 0.0f, 1.0f);
  }
}

void write_setting(float value) {
  const char* base = std::getenv("LOCALAPPDATA");
  if (!base) {
    return;
  }
  CreateDirectoryA((std::string(base) + "\\KCDS").c_str(), nullptr);
  std::ofstream file(setting_path(), std::ios::trunc);
  file << "{\n  \"volume\": " << value << "\n}\n";
}

// The game's browser helper processes (its children).
std::set<DWORD> helper_processes() {
  std::set<DWORD> found;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return found;
  }
  const DWORD self = GetCurrentProcessId();
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Process32FirstW(snapshot, &entry); more;
       more = Process32NextW(snapshot, &entry)) {
    if (entry.th32ParentProcessID == self && _wcsicmp(entry.szExeFile, kHelper) == 0) {
      found.insert(entry.th32ProcessID);
    }
  }
  CloseHandle(snapshot);
  return found;
}

void mark_dirty() {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    dirty = true;
  }
  changed.notify_one();
}

// Raised by Windows when a sound session starts on the watched output: the level is applied at
// once, so a page's first sound is not heard at full volume first.
class SessionWatch : public IAudioSessionNotification {
 public:
  ULONG STDMETHODCALLTYPE AddRef() override {
    return ++references;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --references;
    if (left == 0) {
      delete this;
    }
    return left;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID wanted, void** out) override {
    if (wanted == __uuidof(IUnknown) || wanted == __uuidof(IAudioSessionNotification)) {
      *out = static_cast<IAudioSessionNotification*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl*) override {
    mark_dirty();
    return S_OK;
  }

 private:
  std::atomic<ULONG> references{1};
};

// The default output's session manager, watched for new sessions; made again when the default
// output changes or Windows' audio fails.
struct Output {
  IAudioSessionManager2* manager = nullptr;
  SessionWatch* watch = nullptr;
  std::wstring device;

  void drop() {
    if (manager && watch) {
      manager->UnregisterSessionNotification(watch);
    }
    if (watch) {
      watch->Release();
      watch = nullptr;
    }
    if (manager) {
      manager->Release();
      manager = nullptr;
    }
    device.clear();
  }
};

// The default output's id, empty when there is none.
std::wstring default_output(IMMDevice** out) {
  Held<IMMDeviceEnumerator> devices;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(devices.out()))) ||
      FAILED(devices.pointer->GetDefaultAudioEndpoint(eRender, eConsole, out))) {
    return std::wstring();
  }
  LPWSTR id = nullptr;
  std::wstring text;
  if (SUCCEEDED((*out)->GetId(&id)) && id) {
    text = id;
    CoTaskMemFree(id);
  }
  return text;
}

// Keeps `output` on the current default output; false when there is none.
bool follow_default(Output& output) {
  Held<IMMDevice> device;
  const std::wstring id = default_output(device.out());
  if (id.empty()) {
    output.drop();
    return false;
  }
  if (id == output.device && output.manager) {
    return true;
  }
  output.drop();
  if (FAILED(device.pointer->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&output.manager)))) {
    output.manager = nullptr;
    return false;
  }
  output.device = id;
  output.watch = new SessionWatch();
  // Windows raises session notifications only once the sessions were listed.
  Held<IAudioSessionEnumerator> listed;
  output.manager->GetSessionEnumerator(listed.out());
  output.manager->RegisterSessionNotification(output.watch);
  return true;
}

// Sets `level` on every session of the helpers on the output; how many were set, -1 when
// Windows' audio could not be asked.
int apply(IAudioSessionManager2* manager, float level) {
  const std::set<DWORD> helpers = helper_processes();
  if (helpers.empty()) {
    return 0;
  }
  Held<IAudioSessionEnumerator> sessions;
  int count = 0;
  if (FAILED(manager->GetSessionEnumerator(sessions.out())) ||
      FAILED(sessions.pointer->GetCount(&count))) {
    return -1;
  }
  int set = 0;
  for (int index = 0; index < count; ++index) {
    Held<IAudioSessionControl> control;
    Held<IAudioSessionControl2> details;
    DWORD process = 0;
    const bool known =
        SUCCEEDED(sessions.pointer->GetSession(index, control.out())) &&
        SUCCEEDED(control.pointer->QueryInterface(__uuidof(IAudioSessionControl2),
                                                  reinterpret_cast<void**>(details.out()))) &&
        SUCCEEDED(details.pointer->GetProcessId(&process));
    if (!known || helpers.count(process) == 0) {
      continue;
    }
    Held<ISimpleAudioVolume> volume;
    if (FAILED(control.pointer->QueryInterface(__uuidof(ISimpleAudioVolume),
                                               reinterpret_cast<void**>(volume.out())))) {
      continue;
    }
    float now = -1.0f;
    volume.pointer->GetMasterVolume(&now);
    if (std::fabs(now - level) > 0.001f &&
        SUCCEEDED(volume.pointer->SetMasterVolume(level, nullptr))) {
      set += 1;
    }
  }
  return set;
}

void run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Output output;
  float told = -1.0f;
  for (;;) {
    float wanted = 0.0f;
    {
      std::unique_lock<std::mutex> lock(state_mutex);
      changed.wait_for(lock, kApplyEvery, [] { return dirty; });
      dirty = false;
      read_setting_once();
      wanted = level(player_setting, server_volume, server_muted);
    }
    if (!follow_default(output)) {
      continue;
    }
    const int set = apply(output.manager, wanted);
    if (set < 0) {
      output.drop();
      continue;
    }
    if (log_line && set > 0 && std::fabs(wanted - told) > 0.001f) {
      told = wanted;
      log_line("WEB audio level %.2f on %d session(s)", wanted, set);
    }
  }
}

}  // namespace

void start(Logger logger) {
  std::call_once(started, [logger] {
    log_line = logger;
    std::thread(run).detach();
  });
}

float setting() {
  std::lock_guard<std::mutex> lock(state_mutex);
  read_setting_once();
  return player_setting;
}

void set_setting(float value) {
  const float kept = clamped(value, 0.0f, 1.0f);
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    read_setting_once();
    player_setting = kept;
    dirty = true;
  }
  write_setting(kept);
  changed.notify_one();
}

void set_server(float volume, bool muted) {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    server_volume = clamped(volume, 0.0f, 100.0f);
    server_muted = muted;
    dirty = true;
  }
  changed.notify_one();
}

}  // namespace kcds::runtime::web::audio
