#pragma once
#include "kcd2s/integration/chat.hpp"
#include <cstdint>
#include <string>
#include <vector>
struct ID2D1DeviceContext;
namespace kcd2s::native_chat {
using Logger = void (*)(const char*, ...);
bool install(Logger logger);
bool editing();
void set_ready(bool ready);
// The engine's developer console is open; T then types into the console instead of opening chat.
void set_engine_console_open(bool open);
// The local player is in a crafting minigame (smithing, alchemy); T then opens the game's own help
// there instead of chat.
void set_minigame_open(bool open);
void receive(integration::ChatMessage message);
std::vector<std::string> take_submissions();
// Text capture for Book.write: the input line takes the keys (game actions stay gated while it
// is open) without drawing; Enter adds a line break, Esc ends it.
struct CaptureUpdate {
  bool changed = false;
  bool ended = false;
  std::string text;  // UTF-8, when changed
};
bool begin_capture(const std::string& initial);
void end_capture();
CaptureUpdate take_capture();
// Resource control of the overlay (Chat global). Hidden, no line is drawn and T does
// not open the input; hiding closes an open input.
void set_ui_visible(bool visible);
bool ui_visible();
// The T presses since the last call while the overlay was hidden (the chatKey event).
unsigned take_hidden_chat_keys();
// Opens the input line as T does; false when the overlay is hidden or not ready.
bool open_input();
// Closes the input line without sending it.
void close_input_line();
// Shows a short red damage vignette above every game UI, for hits taken while a full-screen game
// UI hides the native screen blood effect.
void flash_damage();
// A full-screen layer drawn below the chat on every frame while active() is true. Both run on the
// present thread; paint draws between BeginDraw and EndDraw.
struct FrameLayer {
  bool (*active)() = nullptr;
  void (*paint)(ID2D1DeviceContext* canvas, float width, float height, void* window) = nullptr;
};
// The web views: resource views and the server browser page.
void set_view_layer(FrameLayer layer);
// Called on every presented frame with the back buffer size, before anything is drawn.
void set_frame_size_observer(void (*observer)(int width, int height));
// Sees every message of the game window first (a focused web view); returning true consumes it
// with result as the window procedure's return value.
using InputSink = bool (*)(void* window, unsigned message, std::uintptr_t wparam,
                           std::intptr_t lparam, std::intptr_t& result);
void set_input_sink(InputSink sink);

// What closing the game window does (Alt+F4, the title bar's close button): the game
// ignores WM_CLOSE, so the window's close command calls this instead (the main menu's Quit).
using CloseHandler = void (*)();
void set_close_handler(CloseHandler handler);
// The game window (an HWND) once the frame hooks know it; null before.
void* window();
// The mouse wheel turned over the game window since the last call (WM_MOUSEWHEEL deltas, 120 a
// notch, away from you positive); the game still gets every message.
int take_wheel();
}  // namespace kcd2s::native_chat
