#include "../store_copy.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "kcd2s/testkit.hpp"

namespace fs = std::filesystem;
namespace launch = kcds::runtime::launch;

namespace {

fs::path fresh_root(const char* name) {
  const fs::path root = fs::temp_directory_path() / "kcds-store-copy-test" / name;
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "Bin" / "Build");
  return root;
}

void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

}  // namespace

KCD2S_TEST(store_copy, epic_ids_from_a_mancpn_and_a_launcher_manifest) {
  const auto mancpn = launch::parse_epic_ids(
      "{\n\t\"FormatVersion\": 0,\n\t\"AppName\": \"AppOne\",\n"
      "\t\"CatalogItemId\": \"item1\",\n\t\"CatalogNamespace\": \"ns1\"\n}");
  KCD2S_CHECK(mancpn.has_value());
  KCD2S_CHECK_EQ(mancpn->app_name, std::string("AppOne"));
  KCD2S_CHECK_EQ(mancpn->catalog_item, std::string("item1"));
  KCD2S_CHECK_EQ(mancpn->catalog_namespace, std::string("ns1"));

  const std::string item =
      "{\"FormatVersion\":0,\"bIsIncompleteInstall\":false,\"DisplayName\":\"Game\","
      "\"InstallLocation\":\"D:\\\\Epic Games\\\\Game\",\"LaunchExecutable\":"
      "\"Bin/Build/KingdomCome.exe\",\"CatalogNamespace\":\"ns2\",\"CatalogItemId\":\"item2\","
      "\"AppName\":\"AppTwo\"}";
  const auto ids = launch::parse_epic_ids(item);
  KCD2S_CHECK(ids.has_value());
  KCD2S_CHECK_EQ(ids->app_name, std::string("AppTwo"));
  KCD2S_CHECK_EQ(launch::epic_install_location(item), std::string("D:\\Epic Games\\Game"));
}

KCD2S_TEST(store_copy, epic_ids_need_all_three_values) {
  KCD2S_CHECK(!launch::parse_epic_ids("").has_value());
  KCD2S_CHECK(!launch::parse_epic_ids("{\"AppName\":\"A\",\"CatalogItemId\":\"I\"}").has_value());
  KCD2S_CHECK(!launch::parse_epic_ids(
                   "{\"AppName\":\"\",\"CatalogItemId\":\"I\",\"CatalogNamespace\":\"N\"}")
                   .has_value());
  // A key that is only a value elsewhere is not the key.
  KCD2S_CHECK(!launch::parse_epic_ids(
                   "{\"Note\":\"AppName\",\"CatalogItemId\":\"I\",\"CatalogNamespace\":\"N\"}")
                   .has_value());
}

KCD2S_TEST(store_copy, epic_uri_names_namespace_item_and_app) {
  const launch::EpicIds ids{"App Name", "ns", "item"};
  KCD2S_CHECK_EQ(launch::epic_launch_uri(ids),
                 std::string("com.epicgames.launcher://apps/ns%3Aitem%3AApp%20Name?action=launch&"
                             "silent=true"));
}

KCD2S_TEST(store_copy, store_is_read_from_the_install_root) {
  const fs::path steam = fresh_root("steam");
  KCD2S_CHECK(launch::store_of_install(steam) == launch::Store::Steam);

  const fs::path epic = fresh_root("epic");
  write(epic / ".egstore" / "0123.mancpn",
        "{\"AppName\":\"A\",\"CatalogItemId\":\"I\",\"CatalogNamespace\":\"N\"}");
  KCD2S_CHECK(launch::store_of_install(epic) == launch::Store::Epic);
  const auto ids = launch::find_epic_ids(epic, fs::path());
  KCD2S_CHECK(ids.has_value());
  KCD2S_CHECK_EQ(ids->catalog_namespace, std::string("N"));

  // An install without .egstore ids, recorded by the Epic launcher's manifest.
  const fs::path recorded = fresh_root("epic-recorded");
  fs::create_directories(recorded / ".egstore");
  const fs::path program_data = recorded.parent_path() / "ProgramData";
  std::string location = recorded.string();
  std::string escaped;
  for (const char c : location) {
    escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
  }
  write(program_data / "Epic" / "EpicGamesLauncher" / "Data" / "Manifests" / "A.item",
        "{\"InstallLocation\":\"" + escaped +
            "\",\"AppName\":\"Rec\",\"CatalogItemId\":\"RI\",\"CatalogNamespace\":\"RN\"}");
  write(program_data / "Epic" / "EpicGamesLauncher" / "Data" / "Manifests" / "B.item",
        "{\"InstallLocation\":\"C:\\\\Elsewhere\",\"AppName\":\"Other\",\"CatalogItemId\":\"OI\","
        "\"CatalogNamespace\":\"ON\"}");
  const auto recorded_ids = launch::find_epic_ids(recorded, program_data);
  KCD2S_CHECK(recorded_ids.has_value());
  KCD2S_CHECK_EQ(recorded_ids->app_name, std::string("Rec"));
  KCD2S_CHECK(!launch::find_epic_ids(fresh_root("no-ids"), program_data).has_value());

  const fs::path gog = fresh_root("gog");
  write(gog / "goggame-1234567890.info", "{}");
  KCD2S_CHECK(launch::store_of_install(gog) == launch::Store::Gog);

  const fs::path store = fresh_root("msstore");
  write(store / "appxmanifest.xml", "<Package/>");
  write(store / ".egstore" / "x.mancpn", "{}");
  // A package manifest wins: such a copy is never started as a plain process.
  KCD2S_CHECK(launch::store_of_install(store) == launch::Store::MsStore);

  std::error_code ec;
  fs::remove_all(fs::temp_directory_path() / "kcds-store-copy-test", ec);
}
