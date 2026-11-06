#pragma once

namespace kcds::game_client {

// Resolve the UI registry's own mask-registration method and its native by-value string
// cleanup, then keep each remote player out of the local UI listener registry.
bool install_remote_ui_role();

}  // namespace kcds::game_client
