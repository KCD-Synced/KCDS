// World Architect's text and pictures, drawn by the client's own overlay in the font
// the watermark uses (Segoe UI): the game's Scaleform has only its embedded fonts and loads no PNG,
// so the editor's screen (Client/resources/architect/shell.cjs) keeps its panels, clicks and
// cursor in Scaleform and sends what it writes and shows here. Places are on the editor's 1920 by
// 1080 stage, which is stretched to the frame as the editor's movie is: across and down on their
// own, text keeping its shape at the smaller of the two scales.
//
//   { op: 'architectText', runs: [{ x, y, w, h, t, s, c, a, b }] }
//       every line of text the screen shows now (it replaces the ones before): its box, text, size
//       in stage pixels, colour (0xRRGGBB), alignment ('l', 'c' or 'r') and bold.
//   { op: 'architectPictureData', key, offset, total, data }
//       a PNG in base64 pieces; whole, it is decoded once and kept by its key.
//   { op: 'architectPictures', items: [{ key, x, y, w, h }] }
//       the pictures the screen shows now, each fitted into its box.
//   { op: 'architectClear' }
//       the editor closed: nothing is drawn.
//
// A whole picture is also kept on disk (%LOCALAPPDATA%\KCDS\architect\pictures, a file per key
// named by its hash, and index.txt with "<hash> <key>" lines), so a later session has it without
// taking it again: cached_keys() lists them, and a placed key not in memory is read from there.
//
// Requests: any thread (handle_runtime_message). paint(): render thread (chat/chat_overlay.cpp).
#pragma once

#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

struct ID2D1DeviceContext;

namespace kcds::runtime::architect::overlay {

// True when the message was one of the editor's overlay ones.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Draws the editor's pictures and text onto the frame (between BeginDraw and EndDraw).
void paint(ID2D1DeviceContext* canvas, float width, float height);

// The world went: nothing is drawn, the pictures are forgotten.
void reset();

// The keys of the pictures kept on disk. Any thread.
std::vector<std::string> cached_keys();

// The height of the editor's stage (the window's shape, 1080 or more): a window `height` pixels
// high shows one stage pixel as height / stage_height() pixels. Any thread.
float stage_height();

}  // namespace kcds::runtime::architect::overlay
