// Which of the textures the game makes is a web view's screen in the world
// (world_textures.hpp): the texture of a screen model a resource streams, one level of 8-bit sRGB
// BGRA pixels as large as the page, asked for while a view of that size draws into the world.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::render::world_textures {

// The fields of a D3D12_RESOURCE_DESC that tell a screen's texture.
struct TextureAsk {
  std::uint32_t dimension = 0;  // D3D12_RESOURCE_DIMENSION
  std::uint64_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t depth_or_array = 0;
  std::uint32_t mips = 0;
  std::uint32_t format = 0;  // DXGI_FORMAT
  std::uint32_t samples = 0;
  std::uint32_t flags = 0;  // D3D12_RESOURCE_FLAGS
};

struct Size {
  int width = 0;
  int height = 0;
};

constexpr std::uint32_t kTexture2D = 3;
// DXGI_FORMAT_B8G8R8A8_TYPELESS and _UNORM_SRGB: the page's own pixel layout, so its frames copy
// in as they are. Not _UNORM: the web host's own frame copies (D3D11 textures on the game's
// device) are made in it, as large as the page.
constexpr std::uint32_t kBgraFormats[] = {90, 91};

inline bool bgra(std::uint32_t format) {
  for (std::uint32_t known : kBgraFormats) {
    if (format == known) {
      return true;
    }
  }
  return false;
}

// Whether the game's texture `ask` is a screen's: a plain 2D texture (no render target or other
// use, one sample) of BGRA pixels the size of one of the `watched` pages, with one level or its
// mip levels (made again from each page frame).
inline bool is_screen_texture(const TextureAsk& ask, const Size* watched, std::size_t count) {
  const bool plain = ask.dimension == kTexture2D && ask.depth_or_array == 1 && ask.mips >= 1 &&
                     ask.samples == 1 && ask.flags == 0;
  if (!plain || !bgra(ask.format)) {
    return false;
  }
  for (std::size_t at = 0; at < count; ++at) {
    const bool same_width = ask.width == static_cast<std::uint64_t>(watched[at].width);
    const bool same_height = ask.height == static_cast<std::uint32_t>(watched[at].height);
    if (same_width && same_height) {
      return true;
    }
  }
  return false;
}

}  // namespace kcds::runtime::render::world_textures
