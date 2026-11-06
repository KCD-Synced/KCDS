// Gizmos the server draws (kcd2s/integration/gizmos.hpp): lines, spheres, boxes and
// text in the world, kept for their duration and drawn every frame with the game's renderer aux
// geometry, as the script binding System.DrawLine and System.DrawLabel draw (WHGame 1.5.5):
//
//   gEnv->pRenderer  vtable + 1592 GetIRenderAuxGeom (CScriptBind_System::DrawLine)
//   IRenderAuxGeom   + 8 SetRenderFlags(returned flags, flags) with the binding's 0x01000000,
//                    + 64 DrawLine(from, colour, to, colour, thickness) with ColorB colours
//   DrawLabelEx (position, size, ColorF, fixed size, centred, text), what
//                    CScriptBind_System::DrawLabel calls
//
// A sphere is three great circles, a box its twelve edges. What is drawn lasts one frame, so
// every live gizmo is drawn again each frame until it ends. Other features draw their own lines
// for one frame through line() and circle().
//
// The client resources' own drawings (the client Gizmos global, Client/resources/gizmos.cjs) come
// from the resource runtime as gizmoSet, gizmoTransform, gizmoRemove and gizmoRemoveOwner, built
// there into points, lines and triangles. They are drawn with the same aux geometry: lines with
// the drawing's width (DrawLine's thickness), faces with DrawTriangle (IRenderAuxGeom + 136),
// and these render flags, as the aux geometry's state setup reads them:
//
//   0x00800000  no culling, as the state setup reads it; live a flat face was still not drawn
//               from its back (a client mesh seen from above wound counter-clockwise), so
//               a face meant for both sides is given both ways round
//               (Client/resources/tabletop.cjs)
//   0x00400000  no depth write (drawings never write scene depth)
//   0x00200000  no depth test (depthTest: false)
//   0x20000000  blended, for a colour that is not opaque (what DrawPolyline, sets)
//
// Engine thread, but for handle_runtime_message.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/gizmos.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::gizmos {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* const* renderer = nullptr;  // the global holding gEnv->pRenderer
  bool layout_verified = false;     // the DrawLine binding has the slots above
  void* draw_label = nullptr;       // DrawLabelEx
};

// False when a native is missing; nothing is drawn then.
bool install(const Natives& natives, Logger logger);

// Draws, replaces or clears a gizmo; `now` in seconds.
void apply(const kcd2s::integration::GizmoOrder& order, double now);

// Draws every live gizmo and client drawing and forgets the ones whose time is over.
void frame(double now);

// Forgets every gizmo of the server, as the game's level does when it unloads.
void reset();

// A resource runtime message; false when it is not a client drawing's. Any thread: it is queued
// for the next frame.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// A world on `map` begins ("" when the session ends): the client drawings of another level are
// forgotten. True when they were (the session's first world keeps them).
bool level(const std::string& map);

// This frame only: a line, and a flat circle about the vertical through `center`. Colours are
// r, g, b, a from 0 to 1.
void line(const float from[3], const float to[3], const float rgba[4]);
// The same with its thickness in pixels, drawn over the scene (no depth test), blended.
void line(const float from[3], const float to[3], const float rgba[4], float thickness);
void circle(const float center[3], float radius, const float rgba[4]);

// This frame only: a text facing the camera at `position`, centred, of the game's label size
// `size` (as the binding's DrawLabel), one line per line break. Colour r, g, b, a from 0 to 1.
void label(const float position[3], float size, const float rgba[4], const char* text);

// This frame only: faces (index triples into `points`, x, y, z in a row) in `fill` and edges
// (index pairs) in `edge`, colours 0xAARRGGBB, depth tested, as the client drawings are drawn.
void solid(const std::vector<float>& points, const std::vector<std::uint32_t>& triangles,
           const std::vector<std::uint32_t>& lines, std::uint32_t fill, std::uint32_t edge);

}  // namespace kcds::runtime::gizmos
