#include "asset_container.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>

namespace kcds::runtime::assets::container {
namespace {

constexpr std::size_t kNonceAt = 12;
constexpr std::size_t kNonceSize = 12;
constexpr std::size_t kTagAt = 24;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kNameLengthAt = 40;
constexpr std::size_t kNameAt = 42;

using Key = std::array<std::uint8_t, 32>;

Key key_for(const std::string& resource, const std::string& secret) {
  std::string material = "kcds-asset-v1";
  material.push_back('\0');
  material += resource;
  material.push_back('\0');
  material += secret;
  Key key{};
  BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
             reinterpret_cast<PUCHAR>(const_cast<char*>(material.data())), ULONG(material.size()),
             key.data(), ULONG(key.size()));
  return key;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

// The authenticated data: the magic, the version and the resource's name.
std::vector<std::uint8_t> authenticated(const std::vector<std::uint8_t>& sealed,
                                        std::size_t name_length) {
  std::vector<std::uint8_t> data(sealed.begin(), sealed.begin() + kNonceAt);
  data.insert(data.end(), sealed.begin() + kNameAt, sealed.begin() + kNameAt + name_length);
  return data;
}

// AES-256-GCM in place over `bytes`; `encrypt` false decrypts and checks `tag`.
bool aes_gcm(const Key& key, const std::uint8_t* nonce, std::vector<std::uint8_t>& aad,
             std::uint8_t* tag, std::uint8_t* bytes, std::size_t size, bool encrypt) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
    return false;
  }
  auto* gcm = reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM));
  const NTSTATUS mode_set = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, gcm,
                                              ULONG(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0);
  BCRYPT_KEY_HANDLE handle = nullptr;
  NTSTATUS key_made = mode_set;
  if (BCRYPT_SUCCESS(mode_set)) {
    auto* secret = const_cast<PUCHAR>(key.data());
    key_made =
        BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0, secret, ULONG(key.size()), 0);
  }
  bool done = false;
  if (BCRYPT_SUCCESS(key_made)) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = ULONG(kNonceSize);
    info.pbAuthData = aad.data();
    info.cbAuthData = ULONG(aad.size());
    info.pbTag = tag;
    info.cbTag = ULONG(kTagSize);
    ULONG written = 0;
    NTSTATUS status = 0;
    if (encrypt) {
      status = BCryptEncrypt(handle, bytes, ULONG(size), &info, nullptr, 0, bytes, ULONG(size),
                             &written, 0);
    } else {
      status = BCryptDecrypt(handle, bytes, ULONG(size), &info, nullptr, 0, bytes, ULONG(size),
                             &written, 0);
    }
    done = BCRYPT_SUCCESS(status) && written == size;
    BCryptDestroyKey(handle);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return done;
}

std::size_t name_length_of(const std::vector<std::uint8_t>& sealed) {
  return std::size_t(sealed[kNameLengthAt]) | std::size_t(sealed[kNameLengthAt + 1]) << 8;
}

bool is_container(const std::vector<std::uint8_t>& sealed) {
  if (sealed.size() < kNameAt || std::memcmp(sealed.data(), kMagic, sizeof(kMagic)) != 0) {
    return false;
  }
  const std::uint32_t version = std::uint32_t(sealed[8]) | std::uint32_t(sealed[9]) << 8 |
                                std::uint32_t(sealed[10]) << 16 | std::uint32_t(sealed[11]) << 24;
  return version == kVersion && sealed.size() >= kNameAt + name_length_of(sealed);
}

}  // namespace

std::vector<std::uint8_t> seal(const std::vector<std::uint8_t>& pak, const std::string& resource,
                               const std::string& secret) {
  if (resource.size() > 0xFFFF) {
    return {};
  }
  std::vector<std::uint8_t> out(kMagic, kMagic + sizeof(kMagic));
  put_u32(out, kVersion);
  std::uint8_t nonce[kNonceSize] = {};
  const NTSTATUS random =
      BCryptGenRandom(nullptr, nonce, ULONG(sizeof(nonce)), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(random)) {
    return {};
  }
  out.insert(out.end(), nonce, nonce + kNonceSize);
  out.resize(out.size() + kTagSize);
  out.push_back(static_cast<std::uint8_t>(resource.size()));
  out.push_back(static_cast<std::uint8_t>(resource.size() >> 8));
  out.insert(out.end(), resource.begin(), resource.end());
  std::vector<std::uint8_t> aad = authenticated(out, resource.size());
  const std::size_t body_at = out.size();
  out.insert(out.end(), pak.begin(), pak.end());
  if (!aes_gcm(key_for(resource, secret), out.data() + kNonceAt, aad, out.data() + kTagAt,
               out.data() + body_at, pak.size(), true)) {
    return {};
  }
  return out;
}

std::optional<std::string> resource_of(const std::vector<std::uint8_t>& sealed) {
  if (!is_container(sealed)) {
    return std::nullopt;
  }
  return std::string(sealed.begin() + kNameAt, sealed.begin() + kNameAt + name_length_of(sealed));
}

std::optional<std::vector<std::uint8_t>> open(const std::vector<std::uint8_t>& sealed,
                                              const std::string& resource,
                                              const std::string& secret) {
  const auto named = resource_of(sealed);
  if (!named || *named != resource) {
    return std::nullopt;
  }
  const std::size_t body_at = kNameAt + named->size();
  std::vector<std::uint8_t> aad = authenticated(sealed, named->size());
  std::uint8_t tag[kTagSize] = {};
  std::memcpy(tag, sealed.data() + kTagAt, kTagSize);
  std::vector<std::uint8_t> pak(sealed.begin() + body_at, sealed.end());
  if (!aes_gcm(key_for(resource, secret), sealed.data() + kNonceAt, aad, tag, pak.data(),
               pak.size(), false)) {
    return std::nullopt;
  }
  return pak;
}

}  // namespace kcds::runtime::assets::container
