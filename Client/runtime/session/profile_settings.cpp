#include "profile_settings.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"

namespace kcds::game_client {

// applies the logged-in user profile's settings when the profile loads (through the
// listener thunk); it overwrites wh_ui_eulaConfirmedVersion with the profile's value, or
// 0 when the profile has none. If the world started before that, the frontend later showed the
// EULA and then left its main menu over the running world, so the world waits for it.
LoadProfileSettings original_load_profile_settings = nullptr;
std::atomic<bool> profile_settings_loaded{false};
void __fastcall hooked_load_profile_settings(void* self, void* profile) {
  original_load_profile_settings(self, profile);
  if (profile) {
    profile_settings_loaded.store(true);
    log("PROFILE settings applied");
  }
}

}  // namespace kcds::game_client
