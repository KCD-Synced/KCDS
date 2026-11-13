// The running game's install folder (the one holding Data, Localization and kcd.log). A Steam,
// Epic or GOG copy keeps KingdomCome.exe in Bin\<build>\; a Microsoft Store / Xbox app copy keeps
// it in the install folder itself.
#pragma once

#include <filesystem>

namespace kcds::runtime::game_folder {

// The install folder of the game whose KingdomCome.exe is at exe.
std::filesystem::path root_of(const std::filesystem::path& exe);

#ifdef _WIN32
// The running game's install folder; empty when the exe's path is not known.
std::filesystem::path root();
#endif

}  // namespace kcds::runtime::game_folder
