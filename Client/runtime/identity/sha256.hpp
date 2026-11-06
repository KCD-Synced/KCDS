// SHA-256 of a text, in lowercase hex, through Windows' own CNG (the hardware id and the server
// password's proof use it).
#pragma once

#include <filesystem>
#include <string>

namespace kcds::runtime::identity {

// 64 lowercase hex characters, or "" when Windows could not hash.
std::string sha256_hex(const std::string& text);

// SHA-256 of a file's bytes, read in pieces, or "" when it cannot be read or hashed.
std::string sha256_file_hex(const std::filesystem::path& file);

// What the game sends for a server's password: SHA-256 of "<binding>:<password>",
// where binding is the server nonce's licence binding of this handshake. Never the password.
std::string password_proof(const std::string& binding, const std::string& password);

}  // namespace kcds::runtime::identity
