// Render.view (Client/resources/render.cjs): a picture of the world from any camera,
// taken on this machine without the player seeing another view.
//
// One render runs at a time, in phases between the engine thread (the camera) and the render
// thread (the overlay's Present hook, chat/chat_overlay.cpp, before KCDS draws its own overlays):
//
//   1. the render thread keeps a copy of the next frame the player sees (the frame shown while the
//      picture is taken);
//   2. the engine thread makes a camera of its own at the asked pose the active view
//      (camera/anchor_view.hpp; the view that was active, the game's or a shot's, comes back
//      after);
//   3. the render thread lets kFilmPresents frames go by (the engine renders a frame or two
//      behind its update), copies the last one's pixels, and meanwhile presents the kept frame
//      each time, so the player never sees the other view;
//   4. the engine thread ends the camera; the kept frame is presented kRestorePresents more times
//      while the game's own view comes back through the frames in flight;
//   5. the pixels are written as a PNG (WIC), scaled to the asked width, and go to the scripts in
//      pieces (a runtime message holds 64 KiB): { op: 'renderViewChunk', request, data (base64) }
//      each, then { op: 'renderViewDone', request, chunks, width, height }, or { op:
//      'renderViewFailed', request, reason } (the world changed, the view could not be made, a
//      frame format it cannot read, a timeout).
//
// The picture is the game's frame as rendered, its own HUD included (as the player has it).
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

#include "../camera/anchor_view.hpp"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace kcds::runtime::render {

using Logger = void (*)(const char* format, ...);

void install(const camera::Natives& view, Logger logger);

// A script's renderView (any thread): true when the message was one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Engine thread, every frame: starts, films and ends the camera of a render.
void tick(bool in_world, double seconds);

// Render thread, for each presented frame, before KCDS's D3D11 overlays draw on the back buffer:
// the copies go on the game's queue as D3D12 lists of its own, and it never waits for the GPU.
void on_present(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* back_buffer);

// Sends a finished render (or its failure) to the scripts (network thread): the PNG is encoded
// here.
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
void pump(const RuntimeSender& send);

}  // namespace kcds::runtime::render
