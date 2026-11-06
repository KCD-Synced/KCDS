#include "discord_callback.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::identity;

// Discord's sign-in page and the browser's way back.
KCD2S_TEST(discord_login, callback_and_url) {
  DiscordCallback back;
  KCD2S_CHECK(
      parse_discord_callback("GET /discord?code=Ab%2Bc9&state=f00d HTTP/1.1\r\n\r\n", back));
  KCD2S_CHECK(back.code == "Ab+c9" && back.state == "f00d" && back.error.empty());
  DiscordCallback refused;
  KCD2S_CHECK(parse_discord_callback(
      "GET /discord?error=access_denied&error_description=The+user+denied&state=f00d HTTP/1.1",
      refused));
  KCD2S_CHECK(refused.error == "access_denied" && refused.code.empty());
  DiscordCallback other;
  KCD2S_CHECK(!parse_discord_callback("GET /favicon.ico HTTP/1.1", other));
  KCD2S_CHECK(!parse_discord_callback("POST /discord?code=x HTTP/1.1", other));
  KCD2S_CHECK(discord_login_url("123", "f00d") ==
              "https://discord.com/oauth2/authorize?client_id=123&response_type=code&scope="
              "identify&redirect_uri=http%3A%2F%2F127.0.0.1%3A47811%2Fdiscord&state=f00d");
}
