// The KCDS watermark: our mark and the mod's name in the bottom left corner of every
// frame while the player is in a server's loaded world; the main menu, the server browser and the
// loading screen go without it. It is the client's own: no resource,
// server or setting turns it off, moves it or draws over it (the in-frame overlay paints it last,
// after the resources' web views and regardless of the UI toggle). The mark is built into the
// client (generated from branding/assets/kcds-watermark-mark.png), so it does not depend on a file
// next to it.
//
// Render thread (the overlay's Present hook); set_in_world from the engine thread.
#pragma once

#include <string>

struct ID2D1DeviceContext;

namespace kcds::runtime::branding::watermark {

// Paints the watermark onto the frame the canvas draws into (between BeginDraw and EndDraw);
// width and height are the frame's size in pixels.
// Nothing is painted while the player is not in a loaded world.
void paint(ID2D1DeviceContext* canvas, float width, float height);

// Whether the player is in a server's loaded world, from the client's frame (engine thread).
void set_in_world(bool in_world);

// World Architect's editor is open: the watermark is drawn smaller at the
// left end of the editor's menu bar as its logo, in place of the bottom left corner.
// Any thread.
void set_editor(bool open);

// The server line under the name (the design): the server's name and the ping, or
// "localhost" alone for a server on this computer. set_server when the game connects (the name
// still unknown: ""), set_server_name when the server tells it (UTF-8), set_ping every frame (the
// connection's round trip; 0 or less: not known yet). Any thread.
void set_server(const std::wstring& name, bool local);
void set_server_name(const std::string& utf8);
void set_ping(double seconds);

// Where the watermark ended on the last frame it painted, in pixels from the frame's left edge
// (0 when it was not painted or sits in the editor's menu): the voice indicator stands right of it.
// Render thread.
float right_edge();
// The height of its middle on that frame, in pixels from the frame's top. Render thread.
float middle();

}  // namespace kcds::runtime::branding::watermark
