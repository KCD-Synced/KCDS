// The pointer a resource shows (Controls.setCursor, Client/resources/input.cjs): the game's own,
// none, or a picture the resource ships. The game's pointer is a Windows cursor (its hardware
// mouse, r_MouseUseSystemCursor, made from r_MouseCursorTexture), set again on every
// WM_SETCURSOR of its window. While a resource shows the pointer (Controls.showCursor) with a
// style of its own, the game window's WM_SETCURSOR in the client area is answered here: no cursor,
// or the picture's. The mouse stays free and Controls.cursorPosition keeps reporting either way, so
// a resource can draw its own pointer (a wheel menu) without a second one over it. The game's
// own screens and a focused web view keep the game's pointer.
//
// The runtime sends { op: 'cursorStyle', mode: 'game' | 'hidden' | 'image', data?: base64 PNG,
// cached?: download cache hash, hotspotX, hotspotY } when the style in force changes.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::input::cursor_style {

// The runtime's cursorStyle message; false for another. Any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// The game window's WM_SETCURSOR (its hit test); true when the pointer was set here and the
// message is answered (the window's thread).
bool set_cursor(unsigned hit_test);

}  // namespace kcds::runtime::input::cursor_style
