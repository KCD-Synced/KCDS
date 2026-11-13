#pragma once
// Whether a KingdomCome.exe belongs to a Microsoft Store / Xbox app / PC Game Pass install.
// Such a copy is a Windows package: started as a plain process it does not run as the package,
// so the game starts itself again through Windows (without the KCDS client) and closes.
#include <filesystem>
#include <optional>
#include <string>

#include "package_manifest.hpp"

namespace kcds::runtime::launch {

struct StorePackage {
  std::filesystem::path manifest;  // the file the identity was read from
  PackageIdentity identity;
  std::wstring family;     // the package family name; empty when Windows could not compute it
  std::wstring full_name;  // the package installed for this Windows user; empty when there is none
};

// The package the game at game_exe (<root>\Bin\<build>\KingdomCome.exe) belongs to: a package
// manifest in the install root or beside the exe. nullopt for a Steam, Epic or GOG copy.
std::optional<StorePackage> find_store_package(const std::filesystem::path& game_exe);

// "<family>!<application id>", the id Windows starts the package's game by; empty when either
// part is unknown.
std::wstring app_user_model_id(const StorePackage& package);

}  // namespace kcds::runtime::launch
