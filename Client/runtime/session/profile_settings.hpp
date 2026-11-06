// The logged-in user profile's settings: the world waits until the game applied them.
#pragma once

#include <atomic>

namespace kcds::game_client {

// the profile's settings applied (hooked: the original, null without the hook).
using LoadProfileSettings = void(__fastcall*)(void*, void*);
extern LoadProfileSettings original_load_profile_settings;
extern std::atomic<bool> profile_settings_loaded;
void __fastcall hooked_load_profile_settings(void* self, void* profile);
// Without the hook, or if the profile load is never observed, the world starts after this delay
// with the EULA value it can read, as before.
inline constexpr double kProfileSettingsWaitSeconds = 30;

}  // namespace kcds::game_client
