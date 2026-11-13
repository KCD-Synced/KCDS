#pragma once
// The package identity a Microsoft Store / Xbox app copy of the game carries in its install
// folder: appxmanifest.xml (the Windows package manifest) or MicrosoftGame.config (the game's
// own configuration). Plain text parsing, so it is tested on every platform.
#include <optional>
#include <string>
#include <string_view>

namespace kcds::runtime::launch {

struct PackageIdentity {
  std::string name;       // Identity Name, e.g. "Publisher.Game"
  std::string publisher;  // Identity Publisher, the signer's distinguished name
  std::string app_id;     // Application Id (appxmanifest) or Executable Id (MicrosoftGame.config)
};

// The identity in a manifest's text, or nullopt when it has no Identity element with both a
// Name and a Publisher. app_id stays empty when the manifest names no application.
std::optional<PackageIdentity> parse_package_identity(std::string_view manifest);

}  // namespace kcds::runtime::launch
