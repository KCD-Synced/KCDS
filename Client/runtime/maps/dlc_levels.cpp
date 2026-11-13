#include "dlc_levels.hpp"

#include "../identity/licence_pass.hpp"

namespace kcds::runtime::maps::dlc {

// KCD2's DLC Mysteria Ecclesiae, which has the level klaster (Sedlec monastery).
constexpr std::uint32_t kKlasterDlcApp = 3368620;

std::uint32_t app_of(const std::string& level) {
  if (level == "klaster") {
    return kKlasterDlcApp;
  }
  return 0;
}

bool owned(const std::string& level) {
  const std::uint32_t app = app_of(level);
  if (app == 0) {
    return true;
  }
  return identity::steam_dlc_owned(app).value_or(true);
}

}  // namespace kcds::runtime::maps::dlc
