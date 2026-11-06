// The server browser page (on CEF): our HTML menu
// (Client/ui/menu) shown over the native main menu in the game's own DX12 frame.
//
//   view:      an internal web view of Client/ui/web (kcds://kcds/menu/index.html from the ui
//   folder
//              next to the DLL), drawn by the frame hook's view layer above every resource view.
//   input:     while shown the view holds keyboard and mouse (the web views' input routing).
//   messages:  page -> game with sendToGame("kcds:menu", json), game -> page as a "kcds:menu"
//              CustomEvent. The game side protocol is kcd2s::platform::MenuSession.
//
// No engine object is touched here; nothing is written to the game installation.
#pragma once

#include <string>
#include <vector>

namespace kcds::menu {

using Logger = void (*)(const char* format, ...);

struct Options {
  Logger log = nullptr;
};

// Creates the page once the web views are ready (kcds::runtime::web::start runs first). Returns
// false when it could not start; failed() reports a later failure (no browser).
bool start(const Options& options);

// True when the page can no longer be shown.
bool failed();

// True once the page finished loading.
bool page_loaded();

// Shows or hides the page over the frame. While shown, input is forwarded to the page.
void set_visible(bool visible);
bool visible();

// Hides the page for good and closes its view.
void shutdown();

// JSON messages the page sent since the last call.
std::vector<std::string> take_page_messages();

// Sends a JSON message to the page.
void post_to_page(const std::string& json);

}  // namespace kcds::menu
