// Remembered server passwords: kept sealed in the file, given back to the menu page.
#include "remembered_passwords.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "kcd2s/testkit.hpp"

using kcds::runtime::session::passwords::Store;

namespace {

// A stand-in for DPAPI: the bytes reversed and each one changed, so the file never holds the text.
bool seal(const std::string& password, std::vector<std::uint8_t>& sealed) {
  sealed.assign(password.rbegin(), password.rend());
  for (auto& byte : sealed) {
    byte = std::uint8_t(byte ^ 0x5a);
  }
  return true;
}

bool unseal(const std::vector<std::uint8_t>& sealed, std::string& password) {
  password.assign(sealed.rbegin(), sealed.rend());
  for (auto& c : password) {
    c = char(std::uint8_t(c) ^ 0x5a);
  }
  return true;
}

std::string read_all(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

std::string temp_file() {
  return (std::filesystem::temp_directory_path() / "kcds-test-server-passwords.dat").string();
}

}  // namespace

KCD2S_TEST(remembered_passwords, remembered_sealed_and_given_back) {
  const std::string path = temp_file();
  std::remove(path.c_str());
  Store store(path, seal, unseal);
  store.load();
  std::vector<std::string> replies;
  KCD2S_CHECK(
      store.handle(R"({"type":"rememberPassword","key":"id:abc","password":"se\"cret"})", replies));
  KCD2S_CHECK(replies.empty());
  // The file never holds the password as text.
  const std::string file = read_all(path);
  KCD2S_CHECK(file.find("cret") == std::string::npos);
  KCD2S_CHECK(file.find("id:abc") != std::string::npos);
  // A new start reads it back; the page asks for them.
  Store again(path, seal, unseal);
  again.load();
  KCD2S_CHECK(again.handle(R"({"type":"passwords"})", replies));
  KCD2S_CHECK_EQ(replies.size(), std::size_t(1));
  KCD2S_CHECK_EQ(replies[0],
                 std::string(R"({"type":"passwords","passwords":{"id:abc":"se\"cret"}})"));
  // An empty password forgets the key.
  replies.clear();
  KCD2S_CHECK(again.handle(R"({"type":"rememberPassword","key":"id:abc","password":""})", replies));
  Store third(path, seal, unseal);
  third.load();
  KCD2S_CHECK(third.passwords().empty());
  std::remove(path.c_str());
}

KCD2S_TEST(remembered_passwords, other_messages_and_bad_values_are_left) {
  const std::string path = temp_file();
  std::remove(path.c_str());
  Store store(path, seal, unseal);
  std::vector<std::string> replies;
  KCD2S_CHECK(!store.handle(R"({"type":"connect","id":"abc"})", replies));
  KCD2S_CHECK(!store.handle("not json", replies));
  // Taken as the store's, but not kept: a control character, a password too long.
  KCD2S_CHECK(
      store.handle(R"({"type":"rememberPassword","key":"id:a\u0001","password":"x"})", replies));
  KCD2S_CHECK(store.handle(std::string(R"({"type":"rememberPassword","key":"id:b","password":")") +
                               std::string(65, 'p') + "\"}",
                           replies));
  KCD2S_CHECK(store.passwords().empty());
  // A file another Windows user's DPAPI sealed does not unseal: left out.
  {
    std::ofstream file(path, std::ios::binary);
    file << R"({"id:x":"AAAA","id:y":"not base64!"})";
  }
  Store other(path, seal, [](const std::vector<std::uint8_t>&, std::string&) { return false; });
  other.load();
  KCD2S_CHECK(other.passwords().empty());
  std::remove(path.c_str());
}
