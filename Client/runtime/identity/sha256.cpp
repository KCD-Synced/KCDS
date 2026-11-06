#include "sha256.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <fstream>
#include <vector>

namespace kcds::runtime::identity {

namespace {

std::string hex_of(const std::uint8_t (&digest)[32]) {
  static const char kHex[] = "0123456789abcdef";
  std::string hex;
  for (const std::uint8_t byte : digest) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 15]);
  }
  return hex;
}

}  // namespace

std::string sha256_hex(const std::string& text) {
  std::uint8_t digest[32] = {};
  const NTSTATUS hashed = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                     reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
                                     ULONG(text.size()), digest, sizeof(digest));
  if (!BCRYPT_SUCCESS(hashed)) {
    return {};
  }
  return hex_of(digest);
}

std::string sha256_file_hex(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return {};
  }
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (!BCRYPT_SUCCESS(
          BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0))) {
    return {};
  }
  // Read in pieces: a resource file may be hundreds of megabytes.
  std::vector<char> piece(1 << 20);
  bool ok = true;
  while (ok && in) {
    in.read(piece.data(), std::streamsize(piece.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      ok = BCRYPT_SUCCESS(
          BCryptHashData(hash, reinterpret_cast<PUCHAR>(piece.data()), ULONG(got), 0));
    }
  }
  std::uint8_t digest[32] = {};
  ok = ok && in.eof() && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
  BCryptDestroyHash(hash);
  return ok ? hex_of(digest) : std::string();
}

std::string password_proof(const std::string& binding, const std::string& password) {
  return sha256_hex(binding + ":" + password);
}

}  // namespace kcds::runtime::identity
