#include "../game_folder.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::game_folder::root_of;

KCD2S_TEST(game_folder, bin_build_layout_is_two_folders_up) {
  KCD2S_CHECK(root_of("C:/Games/KCD2/Bin/Win64MasterMasterSteamPGO/KingdomCome.exe") ==
              std::filesystem::path("C:/Games/KCD2"));
  KCD2S_CHECK(root_of("C:/Games/KCD2/bin/Win64MasterMasterGogPGO/KingdomCome.exe") ==
              std::filesystem::path("C:/Games/KCD2"));
}

KCD2S_TEST(game_folder, store_layout_keeps_the_exe_in_the_install_folder) {
  KCD2S_CHECK(root_of("D:/XboxGames/Kingdom Come- Deliverance II/Content/KingdomCome.exe") ==
              std::filesystem::path("D:/XboxGames/Kingdom Come- Deliverance II/Content"));
  // A folder only named like Bin's build folder is not mistaken for one.
  KCD2S_CHECK(root_of("C:/Binaries/Game/KingdomCome.exe") ==
              std::filesystem::path("C:/Binaries/Game"));
}
