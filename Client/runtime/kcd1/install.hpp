// Where this PC has Kingdom Come: Deliverance (Steam app 379430), whose own files the KCD1 map is
// built from. Nothing of that game is shipped with KCDS.
//
// Steam's libraries (its libraryfolders.vdf, Steam found in the registry) are searched for the
// game's app manifest; without Steam's list, the game is looked for beside this game's own
// install. KCDS_KCD1_DIR names the folder instead (a test). A folder counts only when every pack
// the map is built from is there.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kcds::runtime::kcd1 {

inline constexpr unsigned kSteamApp = 379430;

// Where the game was looked for: the folder with all its packs, or the first folder Steam names
// for it whose packs are not all there (an install not finished, or files removed), with those
// packs.
struct InstallSearch {
  std::filesystem::path folder;
  std::filesystem::path incomplete;
  std::vector<std::filesystem::path> missing;
};
InstallSearch search_install();

// The game's folder (the one holding Data\), empty when this PC does not have it whole.
std::filesystem::path find_install();

// The packs the map is built from that `folder` lacks (relative to it), empty when it has all.
std::vector<std::filesystem::path> missing_files(const std::filesystem::path& folder);

// The library folders in a libraryfolders.vdf text (its "path" values, UTF-8), in order.
std::vector<std::filesystem::path> library_paths(const std::string& vdf);

// The "installdir" of an app manifest (appmanifest_<app>.acf) text, empty when it has none.
std::string install_folder(const std::string& manifest);

// A path as UTF-8 text (for the log and the build stamp).
std::string utf8(const std::filesystem::path& path);

// A path from UTF-8 text (Steam's files are UTF-8).
std::filesystem::path from_utf8(const std::string& text);

// A path as the plain ASCII text CryPak opens: as it is, else its Windows short (8.3) form when
// that is plain ASCII; empty when neither is (a folder named with other letters on a drive
// without short names).
std::string crypak_path(const std::filesystem::path& path);

}  // namespace kcds::runtime::kcd1
