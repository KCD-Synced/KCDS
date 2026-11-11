#include "../world_texture_match.hpp"

#include "kcd2s/testkit.hpp"

namespace textures = kcds::runtime::render::world_textures;

namespace {

const textures::Size kPages[] = {{1280, 720}, {800, 600}};

textures::TextureAsk screen_ask() {
  textures::TextureAsk ask;
  ask.dimension = textures::kTexture2D;
  ask.width = 1280;
  ask.height = 720;
  ask.depth_or_array = 1;
  ask.mips = 1;
  ask.format = 91;
  ask.samples = 1;
  ask.flags = 0;
  return ask;
}

}  // namespace

KCD2S_TEST(world_texture_match, a_plain_srgb_bgra_texture_of_a_page_size_is_a_screen) {
  KCD2S_CHECK(textures::is_screen_texture(screen_ask(), kPages, 2));
  textures::TextureAsk other = screen_ask();
  other.width = 800;
  other.height = 600;
  other.format = 90;  // typeless
  KCD2S_CHECK(textures::is_screen_texture(other, kPages, 2));
}

KCD2S_TEST(world_texture_match, the_web_hosts_own_frame_copies_are_not) {
  // A page frame's copy on the game's device: B8G8R8A8_UNORM, as large as the page.
  textures::TextureAsk copy = screen_ask();
  copy.format = 87;
  KCD2S_CHECK(!textures::is_screen_texture(copy, kPages, 2));
}

KCD2S_TEST(world_texture_match, other_sizes_formats_and_uses_are_not) {
  textures::TextureAsk ask = screen_ask();
  ask.height = 721;
  KCD2S_CHECK(!textures::is_screen_texture(ask, kPages, 2));
  ask = screen_ask();
  ask.format = 71;  // BC1
  KCD2S_CHECK(!textures::is_screen_texture(ask, kPages, 2));
  ask = screen_ask();
  ask.mips = 11;
  KCD2S_CHECK(!textures::is_screen_texture(ask, kPages, 2));
  ask = screen_ask();
  ask.flags = 1;  // a render target
  KCD2S_CHECK(!textures::is_screen_texture(ask, kPages, 2));
  ask = screen_ask();
  ask.depth_or_array = 6;
  KCD2S_CHECK(!textures::is_screen_texture(ask, kPages, 2));
}

KCD2S_TEST(world_texture_match, nothing_matches_while_no_page_is_watched) {
  KCD2S_CHECK(!textures::is_screen_texture(screen_ask(), kPages, 0));
}
