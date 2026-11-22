#include "cef_host.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "web_protocol.hpp"

namespace kcds::loader::cef {
namespace {

std::mutex start_mutex;
bool started = false;
std::atomic<int> state{0};  // 0 starting or not started, 1 ready, -1 failed
std::wstring runtime_folder;
std::wstring cache_folder;
std::vector<std::string> switches;
void (*log_line)(const char*) = nullptr;

void note(const std::string& line) {
  if (log_line) {
    log_line(line.c_str());
  }
}

// The application of the process's one CEF: the loader's, so that no client's code is called by
// CEF once that client is unloaded. Each client registers its own scheme handlers when CEF is
// ready and clears them before it goes.
class LoaderApp : public CefApp, public CefBrowserProcessHandler {
 public:
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
    kcds::web::protocol::register_schemes(registrar);
  }

  void OnBeforeCommandLineProcessing(const CefString& process_type,
                                     CefRefPtr<CefCommandLine> command_line) override {
    if (!process_type.empty()) {
      return;
    }
    for (const std::string& entry : switches) {
      const std::size_t equals = entry.find('=');
      if (equals == std::string::npos) {
        command_line->AppendSwitch(entry);
      } else {
        command_line->AppendSwitchWithValue(entry.substr(0, equals), entry.substr(equals + 1));
      }
    }
  }

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnContextInitialized() override {
    state.store(1);
    note("CEF ready");
  }

  IMPLEMENT_REFCOUNTING(LoaderApp);
};

DWORD WINAPI cef_thread(void*) {
  SetDllDirectoryW(runtime_folder.c_str());
  const std::wstring library = runtime_folder + L"libcef.dll";
  if (!LoadLibraryW(library.c_str())) {
    note("CEF libcef.dll not loaded (error " + std::to_string(GetLastError()) + ")");
    state.store(-1);
    return 1;
  }
  CefMainArgs arguments(GetModuleHandleW(nullptr));
  CefSettings settings;
  settings.no_sandbox = true;
  settings.windowless_rendering_enabled = true;
  settings.multi_threaded_message_loop = true;
  settings.background_color = CefColorSetARGB(0, 0, 0, 0);
  settings.log_severity = LOGSEVERITY_WARNING;
  CefString(&settings.browser_subprocess_path) = runtime_folder + L"kcds-web-helper.exe";
  CefString(&settings.resources_dir_path) = runtime_folder;
  CefString(&settings.locales_dir_path) = runtime_folder + L"locales";
  CefString(&settings.locale) = "en-US";
  CefString(&settings.root_cache_path) = cache_folder;
  CefString(&settings.cache_path) = cache_folder + L"\\profile";
  CefString(&settings.log_file) = cache_folder + L"\\cef.log";
  if (!CefInitialize(arguments, settings, new LoaderApp(), nullptr)) {
    note("CEF CefInitialize failed");
    state.store(-1);
    return 1;
  }
  note("CEF initialized");
  return 0;  // the multi-threaded message loop keeps CEF running on its own UI thread
}

}  // namespace

int start(const KcdsCefSettings* given, void (*logger)(const char*)) {
  std::lock_guard<std::mutex> lock(start_mutex);
  log_line = logger;
  if (started) {
    return 0;
  }
  if (!given || !given->runtime_folder || !given->cache_folder) {
    return 1;
  }
  started = true;
  runtime_folder = given->runtime_folder;
  cache_folder = given->cache_folder;
  for (std::uint32_t i = 0; i < given->switch_count; ++i) {
    if (given->switches[i]) {
      switches.emplace_back(given->switches[i]);
    }
  }
  HANDLE thread = CreateThread(nullptr, 0, cef_thread, nullptr, 0, nullptr);
  if (!thread) {
    state.store(-1);
    return 1;
  }
  CloseHandle(thread);
  return 0;
}

int ready() {
  return state.load();
}

}  // namespace kcds::loader::cef
