// Connects client resources' Web global (Client/resources/web.cjs) with the game client's web
// views (Client/ui/web).
//
//   runtime -> game:  { op: "web", call, id, ... } view calls and { op: "webFile", ... } answers
//                     to file requests; both stay on this machine.
//   game -> runtime:  { op: "webHost", ready, width, height } when the browser state or frame size
//                     changes, { op: "webEvent", json } for browser* events and page calls, and
//                     { op: "webFileRequest", fileRequest, resource, path } for kcds:// loads.
#pragma once

#include <functional>
#include <string>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::web {

using Logger = void (*)(const char* format, ...);

// Starts CEF (once) and registers the view layer and the frame size observer with the frame
// hooks. module_directory holds the DLL, the CEF runtime and kcds-web-helper.exe.
void start(const std::wstring& module_directory, Logger logger);

// True while a web view holds keyboard and mouse input (the game must not act on it).
bool has_input_focus();

// True while a server's loading page is up (LoadingScreen, such as a login before the world):
// the server browser's window stays hidden so it does not cover the page. Any thread.
bool loading_page_up();

// The join ended (left, lost, refused): no server's loading page is up any more. Any thread.
void forget_loading_page();

// Engine thread, every frame: the view camera for views anchored to world points
// (Web.setViewWorldPosition); valid false outside a world.
void set_view_camera(bool valid, const float* matrix, float fov);

// For local::Client::set_local_runtime_handler: true when the message was a web call.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Sends a message to the resource runtime; false when it is not running.
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

// Passes the web views' reports to the resource runtime of session (any value that changes when
// a new client and runtime replace the old ones). Call after every client tick, on the thread that
// ticks the client.
void pump(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::web
