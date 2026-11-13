#include "gog_report.hpp"

#include <system_error>

namespace kcds::runtime::identity {
namespace {

namespace fs = std::filesystem;

// The report's text is only hex, file names and a certificate subject: escape what JSON needs.
std::string json_text(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (byte < 0x20) {
      constexpr char kHex[] = "0123456789abcdef";
      out += "\\u00";
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0F]);
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

std::string base64url(const std::string& bytes) {
  constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const unsigned value = (static_cast<unsigned char>(bytes[i]) << 16) |
                           (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
    out.push_back(kAlphabet[(value >> 18) & 63]);
    out.push_back(kAlphabet[(value >> 12) & 63]);
    out.push_back(kAlphabet[(value >> 6) & 63]);
    out.push_back(kAlphabet[value & 63]);
  }
  const std::size_t left = bytes.size() - i;
  if (left == 1) {
    const unsigned value = static_cast<unsigned char>(bytes[i]) << 16;
    out.push_back(kAlphabet[(value >> 18) & 63]);
    out.push_back(kAlphabet[(value >> 12) & 63]);
  } else if (left == 2) {
    const unsigned value = (static_cast<unsigned char>(bytes[i]) << 16) |
                           (static_cast<unsigned char>(bytes[i + 1]) << 8);
    out.push_back(kAlphabet[(value >> 18) & 63]);
    out.push_back(kAlphabet[(value >> 12) & 63]);
    out.push_back(kAlphabet[(value >> 6) & 63]);
  }
  return out;
}

}  // namespace

const std::vector<std::string>& gog_crack_marker_names() {
  // Configuration files and folders of the common Steam and GOG Galaxy emulators.
  static const std::vector<std::string> names = {
      "steam_emu.ini",        "SmartSteamEmu.ini", "cream_api.ini",
      "ColdClientLoader.ini", "steam_settings",    "NemirtingasGalaxyEmu.json",
  };
  return names;
}

std::string gog_report_ticket(const GogInstall& install) {
  std::string json = "{\"v\":1,\"exe\":" + json_text(install.exe_sha256) +
                     ",\"whgame\":" + json_text(install.whgame_sha256);
  if (!install.galaxy_sha256.empty()) {
    json += ",\"galaxy\":" + json_text(install.galaxy_sha256);
    if (!install.galaxy_signer.empty()) {
      json += ",\"galaxySigner\":" + json_text(install.galaxy_signer);
    }
  }
  if (!install.markers.empty()) {
    json += ",\"markers\":[";
    for (std::size_t i = 0; i < install.markers.size(); ++i) {
      json += (i ? "," : "") + json_text(install.markers[i]);
    }
    json += "]";
  }
  json += "}";
  return base64url(json);
}

bool is_gog_install(const fs::path& exe) {
  std::error_code ec;
  if (fs::is_regular_file(exe.parent_path() / "Galaxy64.dll", ec)) {
    return true;
  }
  const fs::path root = exe.parent_path().parent_path().parent_path();
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    const std::u8string name = it->path().filename().u8string();
    if (name.rfind(u8"goggame-", 0) == 0 && it->path().extension() == ".info") {
      return true;
    }
  }
  return false;
}

}  // namespace kcds::runtime::identity
