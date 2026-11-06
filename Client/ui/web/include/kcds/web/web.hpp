// Resource web views: Chromium pages owned by client resources,
// drawn into the game's own DX12 frame.
//
//   CEF thread:    CefInitialize (windowless, multi-threaded message loop) in the game client DLL;
//                  kcds-web-helper.exe runs the renderer, GPU and utility processes. Each view is a
//                  windowless browser whose GPU frames (OnAcceleratedPaint) are copied into a
//                  per-view shared texture under a keyed mutex.
//   present thread: paint() copies every visible view's latest frame into a texture of the game's
//                  D3D11On12 device and draws the views by zIndex through the frame hook's Direct2D
//                  context.
//   resources:     kcds://resources/<resource>/<path> is served from the files the resource shipped
//                  to this client: the request goes to the resource runtime (take_file_requests)
//                  and its answer (complete_file) completes the load, or makes it a 404. Each view
//                  is locked to the origin of the URL it was created or last navigated with; other
//                  navigations and page events are refused and reported.
//   bridge:        pages call sendToGame(name, json) (kcds-web-helper.exe injects it); scripts
//   reach
//                  pages with emit(), a CustomEvent on window with the payload in detail.
//
// The resource runtime numbers the views; every call below takes that number. All calls are
// thread-safe; CEF work is posted to the CEF UI thread. Nothing here touches engine objects.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID2D1DeviceContext;

namespace kcds::web {

using Logger = void (*)(const char* format, ...);

struct Options {
  std::wstring runtime_folder;  // libcef.dll, its resources and locales, kcds-web-helper.exe
  std::wstring cache_folder;    // CEF profile and cache, outside the game installation
  Logger log = nullptr;
};

// Starts the CEF thread. Returns false when CEF could not be loaded; failed() reports a later
// failure. Views can be created once ready() is true.
bool start(const Options& options);
bool ready();
bool failed();

// Closes every view and shuts CEF down (process exit).
void shutdown();

struct ViewOptions {
  int x = 0;
  int y = 0;
  int width = 0;  // width and height 0: the view fills the screen and follows its size
  int height = 0;
  int z_index = 0;
  bool visible = true;
  bool focus = false;
};

// View calls return false when the view does not exist (or, for create_view, when CEF is not
// ready or the id is taken). url is a kcds://resources/... or http(s) URL.
bool create_view(int id, const std::string& owner, const std::string& url,
                 const ViewOptions& options);
bool destroy_view(int id);
bool set_visible(int id, bool visible);
bool set_focus(int id, bool focused);
bool set_offscreen(int id, bool offscreen);
bool load_url(int id, const std::string& url);
bool resize_view(int id, int width, int height);
bool move_view(int id, int x, int y);
// Anchors the view to a world point: every frame it is drawn with its top-left corner at the
// point's place on screen, moved by offset_x and offset_y, and not drawn while the point is behind
// the camera or off screen. anchored false lets the view keep its own position again.
bool anchor_view(int id, bool anchored, float x, float y, float z, int offset_x, int offset_y);
// payload_json is a JSON text ("null" when there is no payload).
bool emit(int id, const std::string& event_name, const std::string& payload_json);

// Destroys every view the resource owns (resource stop).
void destroy_owned(const std::string& owner);
// Every resource's view goes, the client's own ("@menu") stays: a new resource runtime numbers its
// views from 1 again, and a server's pages end with the connection.
void destroy_resource_views();

// Messages for the resource runtime, in the order they happened. Each is a JSON object:
//   {"type":"web-event","owner":R,"event":"webViewOpened","args":[{...}]}
//   {"type":"web-call","owner":R,"viewId":N,"name":E,"payload":"<json text>"|null}
std::vector<std::string> take_messages();

// Views of the game client itself (the server browser) use an owner that starts with '@' and
// load kcds://kcds/<path>, served from the ui folder next to the DLL. Their events and page calls
// never go to the resource runtime; the client takes them here.
struct InternalMessage {
  std::string event;    // a browser* event name, or empty for a page call
  std::string name;     // the page call's event name
  std::string payload;  // the page call's payload (JSON text, empty for null)
};
std::vector<InternalMessage> take_internal_messages(const std::string& owner);

// kcds://resources/<resource>/<path> loads waiting for the resource runtime.
struct FileRequest {
  int id = 0;
  std::string resource;
  std::string path;
};
std::vector<FileRequest> take_file_requests();

// The runtime's answer: the file's bytes, or found false for a file the resource does not ship.
void complete_file(int id, bool found, std::string bytes);

// The view that holds keyboard and mouse input, or 0 when the game has them.
// The view holding the player's input: the focused one, 0 for none, and 0 while a focused page
// has been loading for more than 6 s (webViewFailed with ERR_TIMED_OUT then says so).
int focused_view();

// Present thread, every frame: while a view has the focus and the game window is in front, a key
// pressed on the keyboard that no key message brought (the window lost its keyboard focus with an
// Alt+Tab while loading) makes the window take the keyboard focus back on its own
// thread and gives the view its browser focus again.
void keep_keys(void* window);

// The game window's message hook (UI thread): while a view has the focus, mouse, keyboard and text
// input go to that view (in its own coordinates) and not to the game, and the cursor takes the
// page's shape. Returns true when the message was consumed; result is then the window procedure's
// return value. Alt+F4 and window management messages always reach the game.
bool handle_window_message(void* window, unsigned message, std::uintptr_t wparam,
                           std::intptr_t lparam, std::intptr_t& result);

// Engine thread: the view camera anchored views are placed with (ISystem::GetViewCamera: rows of
// right, forward, up and position, then the vertical field of view in radians); valid false
// outside a world, where anchored views are not drawn.
void set_view_camera(bool valid, const float* matrix, float fov);

// Present thread: the frame size, and the frame layer (see kcd2s::native_chat::FrameLayer).
void set_screen_size(int width, int height);
void screen_size(int& width, int& height);
bool any_visible();
void paint(ID2D1DeviceContext* canvas, float width, float height);

}  // namespace kcds::web
