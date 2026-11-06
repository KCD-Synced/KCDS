// The server browser page on CEF: an internal web view (Client/ui/web) instead
// of the former off-screen WebView2 whose screenshots capped the page at about 17 fps. See
// include/kcds/menu/compositor.hpp for the contract the game client relies on.
#include "kcds/menu/compositor.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "kcds/web/web.hpp"

namespace kcds::menu {
namespace {

// Resource views are numbered from 1 by the resource runtime; this id is never one of them.
constexpr int kMenuView = 0x40000000;
constexpr const char* kOwner = "@menu";
constexpr const char* kPageUrl = "kcds://kcds/menu/index.html";
// The page's channel in both directions: sendToGame(kChannel, json) and a CustomEvent on window.
constexpr const char* kChannel = "kcds:menu";
// How long the page waits for CEF before the browser counts as unavailable.
constexpr DWORD kStartTimeoutMs = 30000;

Logger log_line = nullptr;
std::mutex state_mutex;
std::vector<std::string> page_messages;
std::atomic<bool> created{false};
std::atomic<bool> loaded{false};
std::atomic<bool> shown{false};
std::atomic<bool> unavailable{false};

template <typename... Args>
void note(const char* format, Args... args) {
  if (log_line) {
    log_line(format, args...);
  }
}

// Shows or hides the view; while shown it holds keyboard and mouse.
void apply_visibility(bool visible) {
  web::set_visible(kMenuView, visible);
  web::set_focus(kMenuView, visible);
}

DWORD WINAPI create_page(void*) {
  ULONGLONG deadline = GetTickCount64() + kStartTimeoutMs;
  while (!web::ready() && !web::failed() && GetTickCount64() < deadline) {
    Sleep(20);
  }
  web::ViewOptions options;
  options.visible = false;
  options.z_index = 1000000;  // above every resource view
  if (!web::ready() || !web::create_view(kMenuView, kOwner, kPageUrl, options)) {
    note("MENU browser unavailable; the page cannot be shown");
    unavailable.store(true);
    return 1;
  }
  created.store(true);
  if (shown.load()) {
    apply_visibility(true);
  }
  note("MENU page view created");
  return 0;
}

// Takes the view's events and page calls.
void drain() {
  for (web::InternalMessage& message : web::take_internal_messages(kOwner)) {
    if (message.event == "webViewReady") {
      loaded.store(true);
      note("MENU page loaded");
    } else if (message.event == "webViewFailed") {
      note("MENU page failed to load");
    } else if (message.event.empty() && message.name == kChannel) {
      std::lock_guard<std::mutex> lock(state_mutex);
      page_messages.push_back(std::move(message.payload));
    }
  }
}

}  // namespace

bool start(const Options& options) {
  log_line = options.log;
  HANDLE thread = CreateThread(nullptr, 0, create_page, nullptr, 0, nullptr);
  if (!thread) {
    unavailable.store(true);
    return false;
  }
  CloseHandle(thread);
  return true;
}

bool failed() {
  return unavailable.load();
}

bool page_loaded() {
  drain();
  return loaded.load();
}

void set_visible(bool visible) {
  shown.store(visible);
  if (created.load()) {
    apply_visibility(visible);
  }
}

bool visible() {
  return shown.load();
}

void shutdown() {
  shown.store(false);
  if (created.exchange(false)) {
    web::destroy_view(kMenuView);
  }
}

std::vector<std::string> take_page_messages() {
  drain();
  std::lock_guard<std::mutex> lock(state_mutex);
  std::vector<std::string> taken;
  taken.swap(page_messages);
  return taken;
}

void post_to_page(const std::string& json) {
  if (created.load()) {
    web::emit(kMenuView, kChannel, json);
  }
}

}  // namespace kcds::menu
