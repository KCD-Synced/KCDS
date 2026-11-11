// Names shared by the browser side (web_host.cpp) and kcds-web-helper.exe.
#pragma once

#include "include/cef_scheme.h"

namespace kcds::web::protocol {

// kcds://resources/<resource>/<path>: files a resource ships to clients.
inline constexpr const char* kScheme = "kcds";
inline constexpr const char* kResourcesHost = "resources";
// kcds://kcds/<path>: the game client's own pages (the server browser) from its ui folder.
inline constexpr const char* kInternalHost = "kcds";
// kcds://icons/item/<name or GUID>: an item's inventory icon as a .png, from the game.
inline constexpr const char* kIconsHost = "icons";

// Renderer -> browser process messages.
//   kCallEvent:  [0] event name, [1] payload JSON text or null, [2] frame URL
//   kInputFocus: [0] true while an editable element has the focus
inline constexpr const char* kCallEvent = "kcds.sendToGame";
inline constexpr const char* kInputFocus = "kcds.inputFocus";

// kcds:// behaves like https: a standard, secure scheme that pages can fetch from.
inline void register_schemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  registrar->AddCustomScheme(kScheme, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                                          CEF_SCHEME_OPTION_CORS_ENABLED |
                                          CEF_SCHEME_OPTION_FETCH_ENABLED);
}

}  // namespace kcds::web::protocol
