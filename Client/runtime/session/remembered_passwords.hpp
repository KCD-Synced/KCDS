// Server passwords the player chose to remember (the menu's Remember password). The menu page
// keeps them only in memory: the game client keeps them in a file of the player's
// (%LOCALAPPDATA%\KCDS\server-passwords.dat), each one sealed for the Windows user (DPAPI), never
// as plain text. The page asks for them when it opens and tells each change:
//   {"type":"passwords"}                                     ->
//   {"type":"passwords","passwords":{key: password}}
//   {"type":"rememberPassword","key":k,"password":p}         p "" forgets the key
// A key is the page's: "id:<listed server id>" or "address:<host:port>".
//
// Portable (the sealing is given): the unit test builds it.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace kcds::runtime::session::passwords {

using Seal = std::function<bool(const std::string& password, std::vector<std::uint8_t>& sealed)>;
using Unseal = std::function<bool(const std::vector<std::uint8_t>& sealed, std::string& password)>;

inline constexpr std::size_t kMostPasswords = 200;
inline constexpr std::size_t kMostKeyBytes = 300;
inline constexpr std::size_t kMostPasswordBytes = 64;

class Store {
 public:
  Store(std::string path, Seal seal, Unseal unseal);

  // Reads the file; a password that does not unseal (another Windows user's) is left out.
  void load();

  // A page message of the store's: its replies to send to the page. False for any other message.
  bool handle(const std::string& message, std::vector<std::string>& replies);

  // {"type":"passwords","passwords":{...}}: every password, for the page.
  std::string all_message() const;

  const std::map<std::string, std::string>& passwords() const {
    return passwords_;
  }

 private:
  void save() const;

  std::string path_;
  Seal seal_;
  Unseal unseal_;
  std::map<std::string, std::string> passwords_;
};

}  // namespace kcds::runtime::session::passwords
