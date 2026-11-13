#include "../gog_report.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "kcd2s/testkit.hpp"

namespace fs = std::filesystem;
namespace identity = kcds::runtime::identity;

namespace {

// base64url (unpadded) back to bytes, for reading a ticket.
std::string decode(const std::string& text) {
  const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  unsigned value = 0;
  int bits = 0;
  for (const char c : text) {
    value = (value << 6) | static_cast<unsigned>(alphabet.find(c));
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((value >> bits) & 0xFF));
    }
  }
  return out;
}

const std::string kExe(64, '1');
const std::string kWHGame(64, '2');
const std::string kGalaxy(64, '3');

fs::path fresh_game(const char* name) {
  const fs::path root = fs::temp_directory_path() / "kcds-gog-report-test" / name;
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "Bin" / "Win64MasterMasterGogPGO");
  return root;
}

void touch(const fs::path& path) {
  std::ofstream(path, std::ios::binary) << "x";
}

}  // namespace

KCD2S_TEST(gog_report, ticket_is_base64url_json_the_master_reads) {
  identity::GogInstall install{kExe, kWHGame, kGalaxy, "GOG Sp. z o.o.", {}};
  const std::string ticket = identity::gog_report_ticket(install);
  KCD2S_CHECK(ticket.find_first_of("+/=") == std::string::npos);
  KCD2S_CHECK_EQ(decode(ticket), "{\"v\":1,\"exe\":\"" + kExe + "\",\"whgame\":\"" + kWHGame +
                                     "\",\"galaxy\":\"" + kGalaxy +
                                     "\",\"galaxySigner\":\"GOG Sp. z o.o.\"}");
}

KCD2S_TEST(gog_report, ticket_leaves_out_what_the_install_lacks_and_lists_markers) {
  identity::GogInstall install{kExe, kWHGame, "", "", {"steam_emu.ini", "steam_settings"}};
  KCD2S_CHECK_EQ(decode(identity::gog_report_ticket(install)),
                 "{\"v\":1,\"exe\":\"" + kExe + "\",\"whgame\":\"" + kWHGame +
                     "\",\"markers\":[\"steam_emu.ini\",\"steam_settings\"]}");
  // A signer name is JSON-escaped.
  identity::GogInstall quoted{kExe, kWHGame, kGalaxy, "A \"B\"\\C", {}};
  KCD2S_CHECK(decode(identity::gog_report_ticket(quoted)).find("\"A \\\"B\\\"\\\\C\"") !=
              std::string::npos);
}

KCD2S_TEST(gog_report, gog_copy_is_known_by_its_info_file_or_galaxy_library) {
  const fs::path steam = fresh_game("steam");
  const fs::path steam_exe = steam / "Bin" / "Win64MasterMasterGogPGO" / "KingdomCome.exe";
  KCD2S_CHECK(!identity::is_gog_install(steam_exe));

  const fs::path info = fresh_game("info");
  touch(info / "goggame-1234567890.info");
  KCD2S_CHECK(
      identity::is_gog_install(info / "Bin" / "Win64MasterMasterGogPGO" / "KingdomCome.exe"));

  const fs::path galaxy = fresh_game("galaxy");
  touch(galaxy / "Bin" / "Win64MasterMasterGogPGO" / "Galaxy64.dll");
  KCD2S_CHECK(
      identity::is_gog_install(galaxy / "Bin" / "Win64MasterMasterGogPGO" / "KingdomCome.exe"));

  KCD2S_CHECK(!identity::gog_crack_marker_names().empty());
  std::error_code ec;
  fs::remove_all(fs::temp_directory_path() / "kcds-gog-report-test", ec);
}
