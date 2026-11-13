// A GOG copy's licence proof. GOG builds are DRM-free and have no ownership proof the Master can
// check, so the game reports its own install instead: the SHA-256 of its key files, the
// Authenticode signer of GOG's SDK library and the names of known emulator or crack files beside
// the game. The Master decides from that report (and admits a GOG copy at the "detected" level
// at most); the report states no trust level of its own.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kcds::runtime::identity {

struct GogInstall {
  std::string exe_sha256;     // KingdomCome.exe
  std::string whgame_sha256;  // WHGame.dll
  std::string galaxy_sha256;  // Galaxy64.dll; empty when the build has none
  std::string galaxy_signer;  // Galaxy64.dll's Authenticode signer; empty when not validly signed
  std::vector<std::string> markers;
};

// Emulator and crack files that never come with a legitimate copy (file or folder names).
const std::vector<std::string>& gog_crack_marker_names();

// The report the Master reads, as base64url (unpadded) JSON:
// {"v":1,"exe":..,"whgame":..[,"galaxy":..,"galaxySigner":..][,"markers":[..]]}.
std::string gog_report_ticket(const GogInstall& install);

// Whether the game at exe (<root>\Bin\<build>\KingdomCome.exe) is a GOG copy: a goggame-<id>.info
// file in its root, or Galaxy64.dll beside it.
bool is_gog_install(const std::filesystem::path& exe);

#ifdef _WIN32
// The running game's install, read from its folder; empty hashes when a file can't be read.
GogInstall read_gog_install();
#endif

}  // namespace kcds::runtime::identity
