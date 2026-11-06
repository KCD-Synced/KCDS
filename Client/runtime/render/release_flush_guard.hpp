// The game hangs while it unloads a map: a guard on its render-resource release lists.
//
// The renderer keeps two lists of resources to release later, each with its own lock (renderer
// + 8 * (list + 1333)). The list flush holds a list's lock while it
// deletes the resources that are ready to go. Deleting a shader (CShader, 312 bytes, vtable
// deleted) releases the shader's own resource (its + 240), and when
// that was the last reference, CBaseResource::Release hands it to the release
// enqueue, which takes the same list's lock. That lock is not reentrant, so the thread
// stops for good. The render thread then waits on its loading lock, which that thread holds, and
// the main thread waits on the render thread, so the whole game stands while the old map unloads.
// It happens when a map's shaders die with it, which a custom map's own materials do.
//
// The guard hooks both functions. While a thread flushes a list, every release that thread hands
// to the enqueue is kept, and handed to the enqueue right after the flush has let the lock go:
// the resource is released on the next flush, as the game meant.
//
// install: engine thread at startup; the hooks run on any render thread.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::render::release_flush_guard {

using Logger = void (*)(const char* format, ...);

// Hooks the list flush and the release enqueue when both are where WHGame 1.5.5 has them.
void install(std::uint8_t* image, std::size_t image_size, Logger logger);

}  // namespace kcds::runtime::render::release_flush_guard
