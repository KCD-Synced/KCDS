#pragma once
// Which store a game install comes from, read from what each store leaves in the install root,
// and the Epic Games Store's ids for starting a copy through the Epic launcher. The text parsing
// is portable, so it is tested on every platform.
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace kcds::runtime::launch {

enum class Store {
  Steam,    // started directly with the Steam app context (also any install no other store marks)
  Epic,     // <root>\.egstore: started through the Epic launcher, which signs the game in
  Gog,      // <root>\goggame-<id>.info: DRM-free, started directly
  MsStore,  // a Windows package manifest (store_package.hpp)
};

const char* store_name(Store store);

// The store of the install at root (<root>\Bin\<build>\KingdomCome.exe).
Store store_of_install(const std::filesystem::path& root);

// The ids the Epic launcher starts a game by.
struct EpicIds {
  std::string app_name;
  std::string catalog_namespace;
  std::string catalog_item;
};

// The ids in an Epic install's .egstore\*.mancpn file or in an Epic launcher manifest
// (%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item); nullopt when one is missing.
std::optional<EpicIds> parse_epic_ids(std::string_view json);

// The InstallLocation of an Epic launcher manifest, or empty.
std::string epic_install_location(std::string_view json);

// com.epicgames.launcher://apps/<namespace>%3A<item>%3A<app>?action=launch&silent=true
std::string epic_launch_uri(const EpicIds& ids);

// The Epic ids of the install at root: its .egstore\*.mancpn, else the Epic launcher manifest
// (under program_data, the machine's ProgramData folder) whose InstallLocation is root. nullopt
// when neither names them.
std::optional<EpicIds> find_epic_ids(const std::filesystem::path& root,
                                     const std::filesystem::path& program_data);

}  // namespace kcds::runtime::launch
