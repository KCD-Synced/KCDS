#include "../package_manifest.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::launch::parse_package_identity;

KCD2S_TEST(package_manifest, appx_manifest_gives_identity_and_application) {
  const auto identity = parse_package_identity(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">\n"
      "  <Identity Name=\"Example.Game\" Publisher=\"CN=Example &amp; Co, O=Example, C=CZ\"\n"
      "            Version=\"1.5.5.0\" ProcessorArchitecture=\"x64\" />\n"
      "  <Applications>\n"
      "    <Application Id=\"Game\" Executable=\"launcher.exe\" />\n"
      "  </Applications>\n"
      "</Package>\n");
  KCD2S_CHECK(identity.has_value());
  KCD2S_CHECK_EQ(identity->name, std::string("Example.Game"));
  KCD2S_CHECK_EQ(identity->publisher, std::string("CN=Example & Co, O=Example, C=CZ"));
  KCD2S_CHECK_EQ(identity->app_id, std::string("Game"));
}

KCD2S_TEST(package_manifest, game_config_gives_identity_and_executable_id) {
  const auto identity = parse_package_identity(
      "<Game configVersion='1'>\n"
      "  <Identity Name='Example.Game' Publisher='CN=Example' Version='1.0.0.0'/>\n"
      "  <ExecutableList>\n"
      "    <Executable Name='KingdomCome.exe' Id='Game' />\n"
      "  </ExecutableList>\n"
      "</Game>\n");
  KCD2S_CHECK(identity.has_value());
  KCD2S_CHECK_EQ(identity->name, std::string("Example.Game"));
  KCD2S_CHECK_EQ(identity->app_id, std::string("Game"));
}

KCD2S_TEST(package_manifest, attribute_names_match_whole_words_only) {
  // "PublisherDisplayName" and "AppName" are not "Publisher" and "Name".
  const auto identity = parse_package_identity(
      "<Identity PublisherDisplayName=\"X\" AppName=\"Y\" Name=\"Real\" Publisher=\"CN=Real\"/>");
  KCD2S_CHECK(identity.has_value());
  KCD2S_CHECK_EQ(identity->name, std::string("Real"));
  KCD2S_CHECK_EQ(identity->publisher, std::string("CN=Real"));
  KCD2S_CHECK(identity->app_id.empty());
}

KCD2S_TEST(package_manifest, no_identity_or_incomplete_identity_is_not_a_package) {
  KCD2S_CHECK(!parse_package_identity("").has_value());
  KCD2S_CHECK(!parse_package_identity("<Package><Applications/></Package>").has_value());
  KCD2S_CHECK(!parse_package_identity("<Identity Name=\"OnlyName\"/>").has_value());
  KCD2S_CHECK(!parse_package_identity("<IdentityList Name=\"A\" Publisher=\"B\"/>").has_value());
  KCD2S_CHECK(!parse_package_identity("<Identity Name=\"A\" Publisher=\"\"/>").has_value());
  // An unterminated tag or value.
  KCD2S_CHECK(!parse_package_identity("<Identity Name=\"A\" Publisher=\"B").has_value());
}
