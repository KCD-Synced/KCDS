// The KCD2-Synced start splash, applied in memory only.
//
// KingdomCome.exe shows a small splash window while the game opens: dialog 231 of the exe with one
// picture control (id 1001) that shows the exe's bitmap 108. In a process started by our launcher
// the DLL draws our overlay, <dll dir>/assets/kcds-splash-overlay.png (the "Synced" lettering, our
// own artwork with transparency), onto that bitmap and puts the result into the picture control
// when the exe creates the dialog. The exe on disk is never changed and no game artwork is
// shipped: the composite is made from the player's own exe. Without the overlay, or when the
// dialog does not look as expected, the original splash stays.
#ifndef KCDS_RUNTIME_BRANDING_SPLASH_HPP
#define KCDS_RUNTIME_BRANDING_SPLASH_HPP

#include <string>

namespace kcds::runtime::branding::splash {

using Logger = void (*)(const char* format, ...);

// Call from DllMain, before the game's main thread runs (the exe creates the splash first
// thing), after MH_Initialize. install_directory is the DLL's directory with a trailing
// separator. Returns false when the hook could not be installed.
bool install(const std::wstring& install_directory, Logger logger);

}  // namespace kcds::runtime::branding::splash

#endif  // KCDS_RUNTIME_BRANDING_SPLASH_HPP
