#include "kcd2s/platform/server_browser.hpp"

#include <string>

#include "kcd2s/platform/master_client.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s::platform;

// A sample /servers response matching the Go Entry json tags.
static const char* kSample = R"([
  {"id":"a","name":"Alpha Freeroam","address":"1.2.3.4:7777","gamemode":"freeroam",
   "players":12,"maxPlayers":32,"tags":["eu","pvp"],"protocol":1,"lastSeen":"2026-01-01T00:00:00Z"},
  {"id":"b","name":"Bravo Racing","address":"5.6.7.8:7777","gamemode":"racing",
   "players":32,"maxPlayers":32,"tags":["na"],"protocol":1,"lastSeen":"2026-01-01T00:00:01Z"},
  {"id":"c","name":"Charlie Dev","address":"9.9.9.9:7777","gamemode":"freeroam",
   "players":2,"maxPlayers":64,"tags":["eu"],"protocol":2,"lastSeen":"2026-01-01T00:00:02Z"}
])";

KCD2S_TEST(browser, parses_go_shape) {
  auto list = parse_server_list(kSample);
  KCD2S_CHECK(list.has_value());
  if (!list)
    return;
  KCD2S_CHECK_EQ(list->size(), std::size_t{3});
  KCD2S_CHECK_EQ((*list)[0].name, std::string("Alpha Freeroam"));
  KCD2S_CHECK_EQ((*list)[0].players, 12);
  KCD2S_CHECK_EQ((*list)[0].maxPlayers, 32);
  KCD2S_CHECK_EQ((*list)[0].tags.size(), std::size_t{2});
  KCD2S_CHECK_EQ((*list)[0].tags[1], std::string("pvp"));
  KCD2S_CHECK_EQ((*list)[2].protocol, 2);
}

KCD2S_TEST(browser, rejects_malformed) {
  KCD2S_CHECK(!parse_server_list("[ {\"id\":\"a\" ").has_value());  // truncated
  KCD2S_CHECK(!parse_server_list("not json").has_value());
}

KCD2S_TEST(browser, sort_by_players_desc) {
  ServerBrowser b;
  b.set_servers(*parse_server_list(kSample));
  b.set_sort(ServerBrowser::Sort::Players, /*descending=*/true);
  auto v = b.view();
  KCD2S_CHECK_EQ(v.size(), std::size_t{3});
  KCD2S_CHECK_EQ(v[0].players, 32);  // Bravo
  KCD2S_CHECK_EQ(v[2].players, 2);   // Charlie
}

KCD2S_TEST(browser, filters) {
  // hide full
  {
    ServerBrowser b;
    b.set_servers(*parse_server_list(kSample));
    b.set_hide_full(true);
    auto v = b.view();
    KCD2S_CHECK_EQ(v.size(), std::size_t{2});  // Bravo (32/32) hidden
  }
  // protocol compat
  {
    ServerBrowser b;
    b.set_servers(*parse_server_list(kSample));
    b.set_protocol(1);
    KCD2S_CHECK_EQ(b.view().size(), std::size_t{2});  // Charlie (proto 2) hidden
  }
  // required tag
  {
    ServerBrowser b;
    b.set_servers(*parse_server_list(kSample));
    b.set_required_tag("eu");
    KCD2S_CHECK_EQ(b.view().size(), std::size_t{2});  // Alpha + Charlie
  }
  // text filter on gamemode
  {
    ServerBrowser b;
    b.set_servers(*parse_server_list(kSample));
    b.set_text_filter("racing");
    auto v = b.view();
    KCD2S_CHECK_EQ(v.size(), std::size_t{1});
    KCD2S_CHECK_EQ(v[0].id, std::string("b"));
  }
}

KCD2S_TEST(browser, heartbeat_roundtrips_through_encode) {
  ServerEntry e;
  e.id = "srv1";
  e.name = "My Server";
  e.address = "10.0.0.1:7777";
  e.gamemode = "freeroam";
  e.players = 5;
  e.maxPlayers = 48;
  e.protocol = 1;
  e.tags = {"eu", "modded"};
  const std::string body = encode_heartbeat(e);

  // The encoded body must itself parse as a one-element server list with the same fields.
  auto back = parse_server_list("[" + body + "]");
  KCD2S_CHECK(back.has_value());
  if (!back)
    return;
  KCD2S_CHECK_EQ(back->size(), std::size_t{1});
  KCD2S_CHECK_EQ((*back)[0].name, std::string("My Server"));
  KCD2S_CHECK_EQ((*back)[0].maxPlayers, 48);
  KCD2S_CHECK_EQ((*back)[0].tags.size(), std::size_t{2});
  // Without a description or website the heartbeat leaves them out (an older master refuses a
  // field it does not know).
  KCD2S_CHECK(body.find("description") == std::string::npos);
  KCD2S_CHECK(body.find("website") == std::string::npos);
}

KCD2S_TEST(browser, description_and_website_roundtrip) {
  ServerEntry e;
  e.id = "srv1";
  e.name = "Trosky RP";
  e.address = "10.0.0.1:7777";
  e.description = "Medieval roleplay around \"Trosky\" castle.";
  e.website = "https://crownrp.cz";
  e.tags = {"Roleplay", "Voice chat"};
  auto back = parse_server_list("[" + encode_heartbeat(e) + "]");
  KCD2S_CHECK(back.has_value());
  if (!back || back->empty())
    return;
  KCD2S_CHECK_EQ((*back)[0].description, e.description);
  KCD2S_CHECK_EQ((*back)[0].website, e.website);
  KCD2S_CHECK_EQ((*back)[0].tags[1], std::string("Voice chat"));
}

KCD2S_TEST(master_client, fetch_and_heartbeat_with_fake_http) {
  std::string posted_url, posted_body;
  MasterClient mc(
      "https://master.example",
      [](const std::string& url) -> std::optional<std::string> {
        if (url == "https://master.example/servers")
          return std::string(kSample);
        return std::nullopt;
      },
      [&](const std::string& url, const std::string& body) {
        posted_url = url;
        posted_body = body;
        return true;
      });

  auto servers = mc.fetch_servers();
  KCD2S_CHECK(servers.has_value());
  KCD2S_CHECK_EQ(servers->size(), std::size_t{3});

  ServerEntry self;
  self.id = "x";
  self.name = "X";
  KCD2S_CHECK(mc.heartbeat(self));
  KCD2S_CHECK_EQ(posted_url, std::string("https://master.example/heartbeat"));
  KCD2S_CHECK(posted_body.find("\"id\":\"x\"") != std::string::npos);
}
