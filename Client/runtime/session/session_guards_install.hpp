// Optional session guards installed at startup: the client player's unbind guard,
// the multiplayer pause guard, and the native exit with the local monologue.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter(); each guard logs when it is unavailable.
void install_session_guards();

}  // namespace kcds::game_client
