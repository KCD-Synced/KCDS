#include "discord_activity.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::discord;

// KCDS's Discord activity: what each scene shows and what a resource may add (the IPC frames are
// the loader's, Client/loader/tests/discord_ipc_test.cpp).
KCD2S_TEST(discord, activity_per_scene) {
  Presence presence;
  KCD2S_CHECK(activity_json(presence, "0.3.0") ==
              "{\"details\":\"Starting up\",\"assets\":{\"large_image\":\"kcds\","
              "\"large_text\":\"KCDS 0.3.0\"},\"buttons\":[{\"label\":\"KCDS website\","
              "\"url\":\"https://kcdsynced.com\"}]}");
  presence.scene = Scene::Menu;
  presence.since = 1761400000;
  KCD2S_CHECK(activity_json(presence, "0.3.0") ==
              "{\"details\":\"In the main menu\",\"timestamps\":{\"start\":1761400000},"
              "\"assets\":{\"large_image\":\"kcds\",\"large_text\":\"KCDS 0.3.0\"},"
              "\"buttons\":[{\"label\":\"KCDS website\",\"url\":\"https://kcdsynced.com\"}]}");
  presence.scene = Scene::Joining;
  KCD2S_CHECK(activity_json(presence, "0.3.0").find("\"Joining a KCDS server\"") !=
              std::string::npos);
  // A resource's status line and small image show only while playing.
  presence.script.state = "Blue team, round 3";
  presence.script.small_image = "blue";
  presence.script.small_text = "Blue team";
  KCD2S_CHECK(activity_json(presence, "0.3.0").find("Blue") == std::string::npos);
  presence.scene = Scene::Playing;
  presence.server = "Harbor \"RP\"";
  KCD2S_CHECK(
      activity_json(presence, "0.3.0") ==
      "{\"details\":\"Playing on Harbor \\\"RP\\\"\",\"state\":\"Blue team, round 3\","
      "\"timestamps\":{\"start\":1761400000},\"assets\":{\"large_image\":\"kcds\","
      "\"large_text\":\"KCDS 0.3.0\",\"small_image\":\"blue\",\"small_text\":\"Blue "
      "team\"},\"buttons\":[{\"label\":\"KCDS website\",\"url\":\"https://kcdsynced.com\"}]}");
}

// A server's name, logo and button: only while playing, KCDS's button stays last.
KCD2S_TEST(discord, server_branding) {
  Presence presence;
  presence.scene = Scene::Menu;
  presence.script.name = "Example";
  presence.script.large_image = "https://example.com/logo.png";
  presence.script.large_text = "Example";
  presence.script.button_label = "Our Discord";
  presence.script.button_url = "https://discord.gg/example";
  const std::string menu = activity_json(presence, "0.3.4");
  KCD2S_CHECK(menu.find("In the main menu") != std::string::npos);
  KCD2S_CHECK(menu.find("Example") == std::string::npos);
  KCD2S_CHECK(menu.find("\"large_image\":\"kcds\"") != std::string::npos);
  presence.scene = Scene::Playing;
  presence.server = "Harbor";
  KCD2S_CHECK(activity_json(presence, "0.3.4") ==
              "{\"details\":\"KCDS - Example\",\"assets\":{\"large_image\":"
              "\"https://example.com/logo.png\",\"large_text\":\"Example\"},\"buttons\":["
              "{\"label\":\"Our Discord\",\"url\":\"https://discord.gg/example\"},"
              "{\"label\":\"KCDS website\",\"url\":\"https://kcdsynced.com\"}]}");
  // A button Discord would refuse is left out; KCDS's stays.
  presence.script.button_url = "http://example.com";
  KCD2S_CHECK(activity_json(presence, "0.3.4").find("Our Discord") == std::string::npos);
  presence.script.button_url = "https://discord.gg/example";
  presence.script.button_label = std::string(40, 'x');
  KCD2S_CHECK(activity_json(presence, "0.3.4").find("xxxx") == std::string::npos);
  KCD2S_CHECK(activity_json(presence, "0.3.4").find("KCDS website") != std::string::npos);
  // No name: KCDS's own line.
  presence.script = ScriptPresence{};
  KCD2S_CHECK(activity_json(presence, "0.3.4").find("\"Playing on Harbor\"") != std::string::npos);
}

KCD2S_TEST(discord, fields_cut_and_escaped) {
  KCD2S_CHECK(field_text("a").empty());
  KCD2S_CHECK(field_text("ab") == "ab");
  KCD2S_CHECK(field_text(std::string(200, 'x')).size() == kMaxFieldBytes);
  // A two-byte character across the limit is dropped whole.
  const std::string cut = field_text(std::string(127, 'x') + "\xC5\x99" + "yy");
  KCD2S_CHECK(cut == std::string(127, 'x'));
  KCD2S_CHECK(json_string("a\nb\\") == R"("a\u000ab\\")");
}

// The server's player count: "(5 of 32)" after the status line while playing.
KCD2S_TEST(discord, player_count) {
  Presence presence;
  presence.scene = Scene::Playing;
  presence.server = "Harbor";
  presence.players = 5;
  presence.max_players = 32;
  std::string json = activity_json(presence, "0.3.4");
  KCD2S_CHECK(json.find("\"state\":\"Players online\"") != std::string::npos);
  KCD2S_CHECK(json.find("\"party\":{\"id\":\"kcds\",\"size\":[5,32]}") != std::string::npos);
  // A resource's status line stands beside the count.
  presence.script.state = "Working as a blacksmith";
  json = activity_json(presence, "0.3.4");
  KCD2S_CHECK(json.find("\"state\":\"Working as a blacksmith\"") != std::string::npos);
  KCD2S_CHECK(json.find("\"size\":[5,32]") != std::string::npos);
  // Kept within what Discord takes: 1 to the places.
  presence.players = 40;
  KCD2S_CHECK(activity_json(presence, "0.3.4").find("\"size\":[32,32]") != std::string::npos);
  // Not known, or not playing: no count.
  presence.max_players = 0;
  KCD2S_CHECK(activity_json(presence, "0.3.4").find("party") == std::string::npos);
  presence.max_players = 32;
  presence.scene = Scene::Menu;
  KCD2S_CHECK(activity_json(presence, "0.3.4").find("party") == std::string::npos);
}
