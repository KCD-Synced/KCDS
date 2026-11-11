// Web views drawn into the world (Web.setViewWorldScreen with texture): a resource streams a
// screen model whose material's texture is one level of BGRA pixels as large as the page. When
// the game makes that texture, it is made a texture of its own instead (the same description, on
// its own memory, open to every queue at once), kept here by its size; the web host copies each
// page frame into it before the overlays draw, and the game draws the screen as any other model:
// behind the player's hands, walls and people, lit by the world.
#pragma once

struct ID3D12Device;
struct ID3D12Resource;

namespace kcds::runtime::render::world_textures {

using Logger = void (*)(const char* format, ...);

// Render thread, once the game's device is known: watches the textures it makes from now on.
void watch(ID3D12Device* device, Logger logger);

// A page of this size draws into the world: the game's next screen texture of this size is its.
// Up to eight sizes are watched; a size stays watched.
void want(int width, int height);

// The latest screen texture of this size, AddRef'd into *out; false when there is none.
bool latest(int width, int height, ID3D12Resource** out);

}  // namespace kcds::runtime::render::world_textures
