// KCDS branding and startup settings, applied by the injected DLL at runtime.
//
// The game installation is never written to. Only a process started by our launcher loads this
// DLL, so a game started from Steam stays vanilla. Inside our process the DLL:
//
//   - mounts <dll dir>/assets/kcds-branding.pak (our main-menu logo) through the engine's own
//     CryPak, right after the engine opened the paks of the installed mods;
//   - feeds the startup settings (skip intro and startup video) to the engine's config sink right
//     after it loaded user.cfg, as if they were extra user.cfg lines;
//   - sets the main-menu build text to "Synced" after the engine wrote its version there.
//
// Every engine function is found by a unique WHGame 1.5.5 signature. When a signature or the pak
// is missing, the DLL logs it and the game keeps its vanilla visuals.
#ifndef KCDS_RUNTIME_BRANDING_HPP
#define KCDS_RUNTIME_BRANDING_HPP

#include <cstddef>
#include <string>

namespace kcds::runtime::branding {

using Logger = void (*)(const char* format, ...);

// Call from DllMain, before the game's main thread runs: the game loads WHGame.dll later with
// LoadLibraryA, and the engine hooks must be in place before the engine starts. install_directory
// is the DLL's directory with a trailing separator. Returns false when the loader watch could not
// be installed (the game then starts vanilla).
bool start(const std::wstring& install_directory, Logger logger);

// Call from the initialization thread once WHGame.dll is loaded. Installs the engine hooks when
// the loader watch missed the load (for example after injection into a running game); the engine
// may then have passed the points already, which is logged.
void ensure_installed();

// Logs the current values of our startup settings. Call once from the engine update thread.
void report_applied_settings();

// The text of an engine console variable ("g_language"); "" when the console or the variable is
// not there. Engine thread.
std::string cvar_value(const char* name);

// Other parts of the client's say over what the game's LocalizeLabel answers (the hook here is the
// only one on it): each is called for every label, in the order added, before the game's own
// lookup; true when it wrote the label's text into `out` (a CryString). Any thread the game
// localizes on. At most kMaxLabelOverrides; false when they are taken.
using LabelOverride = bool (*)(const char* label, void* out);
inline constexpr std::size_t kMaxLabelOverrides = 4;
bool add_label_override(LabelOverride label_override);

// The game's text for a localization key ("nelz_jindrich__kde_nic_ne_FJoo", without the "@") in
// the player's language, through the game's own LocalizeLabel (the overrides above left out); ""
// when the game does not know the key or has not localized anything yet. Any thread the game
// localizes on.
std::string localize(const std::string& key);

}  // namespace kcds::runtime::branding

#endif  // KCDS_RUNTIME_BRANDING_HPP
