#include "remembered_passwords.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#include "../files/base64.hpp"
#include "kcd2s/platform/server_browser.hpp"
#include "kcds/launcher/json.hpp"

namespace kcds::runtime::session::passwords {
namespace {

namespace json = kcds::launcher::json;

// Printable text only: a key or a password the page could send.
bool printable(const std::string& text, std::size_t most) {
  if (text.empty() || text.size() > most) {
    return false;
  }
  for (const unsigned char c : text) {
    if (c < 0x20 || c == 0x7f) {
      return false;
    }
  }
  return true;
}

}  // namespace

Store::Store(std::string path, Seal seal, Unseal unseal)
    : path_(std::move(path)), seal_(std::move(seal)), unseal_(std::move(unseal)) {
}

void Store::load() {
  passwords_.clear();
  std::ifstream file(path_, std::ios::binary);
  if (!file) {
    return;
  }
  std::stringstream text;
  text << file.rdbuf();
  json::Value all;
  std::string error;
  if (!json::parse(text.str(), json::Limits{}, all, error) || !all.is_object()) {
    return;
  }
  for (const auto& [key, value] : all.members()) {
    if (passwords_.size() >= kMostPasswords || !printable(key, kMostKeyBytes) ||
        !value.is_string()) {
      continue;
    }
    std::vector<std::uint8_t> sealed;
    std::string password;
    if (files::decode_base64(value.string(), sealed) && unseal_(sealed, password) &&
        printable(password, kMostPasswordBytes)) {
      passwords_[key] = password;
    }
  }
}

void Store::save() const {
  std::string out = "{";
  bool first = true;
  for (const auto& [key, password] : passwords_) {
    std::vector<std::uint8_t> sealed;
    if (!seal_(password, sealed)) {
      continue;
    }
    out += first ? "\"" : ",\"";
    first = false;
    kcd2s::platform::append_json_string(out, key);
    out += "\":\"" + files::encode_base64(sealed) + "\"";
  }
  out += "}";
  // Replaced whole: written beside it, then renamed over it.
  const std::string temp = path_ + ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) {
      return;
    }
    file << out;
  }
  std::remove(path_.c_str());
  std::rename(temp.c_str(), path_.c_str());
}

std::string Store::all_message() const {
  std::string out = R"({"type":"passwords","passwords":{)";
  bool first = true;
  for (const auto& [key, password] : passwords_) {
    out += first ? "\"" : ",\"";
    first = false;
    kcd2s::platform::append_json_string(out, key);
    out += "\":\"";
    kcd2s::platform::append_json_string(out, password);
    out += "\"";
  }
  return out + "}}";
}

bool Store::handle(const std::string& message, std::vector<std::string>& replies) {
  json::Value parsed;
  std::string error;
  if (!json::parse(message, json::Limits{}, parsed, error) || !parsed.is_object()) {
    return false;
  }
  const std::string type = parsed.member("type").string();
  if (type == "passwords") {
    replies.push_back(all_message());
    return true;
  }
  if (type != "rememberPassword") {
    return false;
  }
  const std::string key = parsed.member("key").string();
  const std::string password = parsed.member("password").string();
  if (!printable(key, kMostKeyBytes)) {
    return true;
  }
  if (password.empty()) {
    if (passwords_.erase(key) != 0) {
      save();
    }
    return true;
  }
  if (!printable(password, kMostPasswordBytes)) {
    return true;
  }
  if (passwords_.count(key) == 0 && passwords_.size() >= kMostPasswords) {
    return true;
  }
  const auto found = passwords_.find(key);
  if (found == passwords_.end() || found->second != password) {
    passwords_[key] = password;
    save();
  }
  return true;
}

}  // namespace kcds::runtime::session::passwords
