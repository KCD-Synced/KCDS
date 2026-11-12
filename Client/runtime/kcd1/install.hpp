// Where this PC has Kingdom Come: Deliverance (Steam app 379430), whose own files the KCD1 map is
// built from. Nothing of that game is shipped with KCDS.
//
// Steam's libraries (its libraryfolders.vdf, Steam found in the registry) are searched for the
// game's app manifest; without Steam's list, the game is looked for beside this game's own
// install. KCDS_KCD1_DIR names the folder instead (a test).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kcds::runtime::kcd1 {

inline constexpr unsigned kSteamApp = 379430;

// The game's folder (the one holding Data\), empty when this PC does not have it.
std::filesystem::path find_install();

// The library folders in a libraryfolders.vdf text (its "path" values), in order.
std::vector<std::filesystem::path> library_paths(const std::string& vdf);

// The "installdir" of an app manifest (appmanifest_<app>.acf) text, empty when it has none.
std::string install_folder(const std::string& manifest);

}  // namespace kcds::runtime::kcd1
