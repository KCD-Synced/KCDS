#pragma once

#include "licence_pass.hpp"

namespace kcds::runtime::identity {

// Observe the game's already-loaded EOS SDK. Safe to retry if the module was not
// loaded on an earlier call. This does not initialize EOS or authenticate a user.
bool install_epic_licence_hooks(Logger log);

// Call from a join worker, never from the game's EOS tick thread. Returns a fresh
// ownership JWT for Master verification, or an empty string on unavailable proof.
// No launcher tokens, exchange codes, credential files, or login UI are accessed.
std::string epic_licence_token(Logger log);

// The game's logged-in Epic account id (32 hex characters), "" before the game's EOS tick has seen
// one or on another platform. For bug reports, which name the player's ids as the bans do.
std::string epic_account_id();

}  // namespace kcds::runtime::identity
