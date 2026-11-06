// The signature cache of this game and client on disk: %LOCALAPPDATA%\KCDS\signatures.txt. The
// first start after a game or client update scans the module; later ones go straight to the world.
#pragma once

#include <cstddef>
#include <optional>

namespace kcds::game_client {

// Opens the cache for the loaded game module, before the first signature is resolved.
void open_signature_store();

// Where a signature matched at an earlier start, and where it matched now.
std::optional<std::size_t> known_signature(const char* pattern);
void remember_signature(const char* pattern, std::size_t offset);
void count_signature(bool known);

// Writes what this start found and logs how long the start took, after the last install.
void save_signature_store();

}  // namespace kcds::game_client
