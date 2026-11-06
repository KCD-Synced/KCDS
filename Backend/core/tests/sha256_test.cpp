#include "kcd2s/core/crypto/sha256.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

using namespace kcd2s::core::crypto;

namespace {

std::string hex(const Sha256Digest& digest) {
  static const char* digits = "0123456789abcdef";
  std::string text;
  for (const std::uint8_t byte : digest) {
    text.push_back(digits[byte >> 4]);
    text.push_back(digits[byte & 15]);
  }
  return text;
}

const std::uint8_t* bytes(const std::string& text) {
  return reinterpret_cast<const std::uint8_t*>(text.data());
}

}  // namespace

// FIPS 180-2 appendix B examples.
KCD2S_TEST(sha256, known_vectors) {
  KCD2S_CHECK_EQ(hex(sha256(bytes(""), 0)),
                 std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  const std::string abc = "abc";
  KCD2S_CHECK_EQ(hex(sha256(bytes(abc), abc.size())),
                 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  KCD2S_CHECK_EQ(hex(sha256(bytes(two_blocks), two_blocks.size())),
                 std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

// Fed in pieces, the hash is the same as in one go (a million 'a's, FIPS 180-2 B.3).
KCD2S_TEST(sha256, streaming_matches) {
  Sha256 hash;
  const std::string chunk(1000, 'a');
  for (int i = 0; i < 1000; ++i) {
    hash.update(bytes(chunk), chunk.size());
  }
  KCD2S_CHECK_EQ(hex(hash.finish()),
                 std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

// RFC 4231 test cases 1, 2 and 6 (a key longer than the block).
KCD2S_TEST(sha256, hmac_rfc4231) {
  const std::vector<std::uint8_t> key1(20, 0x0b);
  const std::string data1 = "Hi There";
  KCD2S_CHECK_EQ(hex(hmac_sha256(key1.data(), key1.size(), bytes(data1), data1.size())),
                 std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
  const std::string key2 = "Jefe";
  const std::string data2 = "what do ya want for nothing?";
  KCD2S_CHECK_EQ(hex(hmac_sha256(bytes(key2), key2.size(), bytes(data2), data2.size())),
                 std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
  const std::vector<std::uint8_t> key6(131, 0xaa);
  const std::string data6 = "Test Using Larger Than Block-Size Key - Hash Key First";
  KCD2S_CHECK_EQ(hex(hmac_sha256(key6.data(), key6.size(), bytes(data6), data6.size())),
                 std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

KCD2S_TEST(sha256, constant_time_compare) {
  const std::uint8_t a[4] = {1, 2, 3, 4};
  const std::uint8_t b[4] = {1, 2, 3, 5};
  KCD2S_CHECK(equal_in_constant_time(a, a, 4));
  KCD2S_CHECK(!equal_in_constant_time(a, b, 4));
  KCD2S_CHECK(equal_in_constant_time(a, b, 3));
}
