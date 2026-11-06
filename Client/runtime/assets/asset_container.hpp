// The KCDS asset container, .kcdsasset (Asset/Animation Studio): a resource's
// streamed assets (meshes, materials, textures, skeletons, animations, physics) packed as one
// zip pak and sealed so that only KCDS opens it. KCDS opens it in memory and serves the pak to
// the game from there (assets/memory_paks); the plain pak is never written to disk.
//
// Layout, all numbers little endian:
//   0   8  "KCDSAST1"
//   8   4  format version, 1
//   12  12 the nonce, new for every seal
//   24  16 the AES-GCM tag
//   40  2  length N of the resource's name
//   42  N  the resource's name (UTF-8)
//   42+N   the zip pak, encrypted with AES-256-GCM; the authenticated data is bytes 0..11 and
//          the name, so a container renamed to another resource or version does not open
//
// The key: SHA-256 of "kcds-asset-v1", 0, the resource's name, 0, the resource's secret. The
// server hands the secret of each resource to the players it lets in; without it the bytes are
// noise to the plain game and to other mods, and a copied cache folder alone does not open.
//
// The honest limit: KCDS itself must decrypt to show the asset, so someone who reads the game's
// memory while it runs can take the decrypted pak. The container stops casual extraction and
// reuse, not a determined attacker with a debugger.
//
// Windows' own CNG does the cryptography (BCrypt AES-GCM, SHA-256, random nonces).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kcds::runtime::assets::container {

inline constexpr char kMagic[8] = {'K', 'C', 'D', 'S', 'A', 'S', 'T', '1'};
inline constexpr std::uint32_t kVersion = 1;

// Seals `pak` for `resource` with its secret; empty when Windows could not encrypt or the name
// is longer than 65535 bytes.
std::vector<std::uint8_t> seal(const std::vector<std::uint8_t>& pak, const std::string& resource,
                               const std::string& secret);

// The resource's name a container names, without opening it; nullopt when it is no container.
std::optional<std::string> resource_of(const std::vector<std::uint8_t>& sealed);

// The pak inside, when `sealed` is a container of `resource` sealed with `secret` and nothing
// in it was changed; nullopt otherwise.
std::optional<std::vector<std::uint8_t>> open(const std::vector<std::uint8_t>& sealed,
                                              const std::string& resource,
                                              const std::string& secret);

}  // namespace kcds::runtime::assets::container
