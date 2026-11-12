// Silent licence proof for one server handshake. Uses only the running game's
// online platform session; never opens a browser or reads launcher credentials.
// Blocking on a dedicated worker: the game thread continues pumping SDK callbacks.
#pragma once

#include <string>
#include <cstdint>

namespace kcds::runtime::identity {

using Logger = void (*)(const char* format, ...);

// A display hint until the server derives authoritative identity from a signed pass.
std::string steam_id();

// Why no pass could be had, for the player: the Master's central ban list, or
// this copy of the game could not be proven an original one. No server admits the player then.
struct LicenceRefusal {
  bool banned = false;
  std::string text;
};

// binding is the 16-character lowercase hex server handshake nonce; address the server's
// "ip:port" as dialled, the pass's audience when the Master does not list it. Returns an
// empty string on unavailable/unsupported proof or refusal, which fails admission. When the
// copy cannot be proven or the Master's central ban list refuses the player, refusal gets what to
// tell them (an unreachable Master is not a refusal: the server says why then).
// kcd1: the server has the Kingdom Come: Deliverance map (it refused with
// RefusalReason::Kcd1Required); the Master checks the player owns that game on Steam and the pass
// says so, or refusal says why not.
std::string licence_pass(const std::string& master_url, const std::string& address,
                         const std::string& listing, const std::string& binding, Logger log,
                         std::int64_t* expires = nullptr, LicenceRefusal* refusal = nullptr,
                         bool kcd1 = false);

}  // namespace kcds::runtime::identity
