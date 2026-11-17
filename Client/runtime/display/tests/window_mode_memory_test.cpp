#include "../window_mode_memory.hpp"

#include "kcd2s/testkit.hpp"

namespace display = kcds::runtime::display;

KCD2S_TEST(window_mode_memory, the_profile_keeps_the_vanilla_mode_while_borderless) {
  display::WindowModeMemory memory;
  memory.vanilla = display::kWindowed;
  const auto save = display::on_profile_save(display::kBorderlessWindow, memory);
  KCD2S_CHECK(save.saved == display::kWindowed);
  KCD2S_CHECK(save.memory.borderless);
  KCD2S_CHECK(save.memory.vanilla == display::kWindowed);
}

KCD2S_TEST(window_mode_memory, a_mode_of_the_game_menu_is_saved_and_remembered) {
  display::WindowModeMemory memory;
  memory.borderless = true;
  memory.vanilla = display::kWindowed;
  const auto save = display::on_profile_save(display::kBorderlessFullScreen, memory);
  KCD2S_CHECK(save.saved == display::kBorderlessFullScreen);
  KCD2S_CHECK(!save.memory.borderless);
  KCD2S_CHECK(save.memory.vanilla == display::kBorderlessFullScreen);
}

KCD2S_TEST(window_mode_memory, the_game_starts_borderless_when_chosen) {
  display::WindowModeMemory memory;
  memory.borderless = true;
  memory.vanilla = display::kBorderlessFullScreen;
  // The profile says windowed: the player changed it in a game without KCDS.
  const auto start = display::on_game_start(display::kWindowed, memory);
  KCD2S_CHECK(start.window_type == display::kBorderlessWindow);
  KCD2S_CHECK(start.memory.vanilla == display::kWindowed);
}

KCD2S_TEST(window_mode_memory, the_game_starts_as_its_profile_says_otherwise) {
  display::WindowModeMemory memory;
  const auto start = display::on_game_start(display::kWindowed, memory);
  KCD2S_CHECK(start.window_type == display::kWindowed);
  KCD2S_CHECK(!start.memory.borderless);
}

KCD2S_TEST(window_mode_memory, a_borderless_profile_falls_back_to_the_remembered_mode) {
  display::WindowModeMemory memory;
  memory.vanilla = display::kWindowed;
  const auto start = display::on_game_start(display::kBorderlessWindow, memory);
  KCD2S_CHECK(start.window_type == display::kWindowed);
  const auto save = display::on_profile_save(display::kBorderlessWindow, start.memory);
  KCD2S_CHECK(save.saved == display::kWindowed);
}

KCD2S_TEST(window_mode_memory, the_file_text_reads_back) {
  display::WindowModeMemory memory;
  memory.borderless = true;
  memory.vanilla = display::kWindowed;
  KCD2S_CHECK(display::from_text(display::to_text(memory)) == memory);
  KCD2S_CHECK(display::from_text("") == display::WindowModeMemory{});
  // A borderless window is never the vanilla mode.
  const auto odd = display::from_text("{\"borderless\": false, \"vanilla\": 1}");
  KCD2S_CHECK(odd.vanilla == display::kBorderlessFullScreen);
}
