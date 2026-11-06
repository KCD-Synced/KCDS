#include "kcd2s/platform/frontend_menu.hpp"

#include <optional>
#include <string>

#include "kcd2s/platform/master_client.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::platform::FrontendMenu;
using kcd2s::platform::MasterClient;
using State = kcd2s::platform::FrontendMenu::State;

namespace {

// A directory with a protocol-1 server (eu-1), a protocol-2 server (old-1), and a full one (na-1).
const char* kServers = R"([
  {"id":"eu-1","name":"Rattay","address":"eu.example:27015","gamemode":"freeroam",
   "players":5,"maxPlayers":32,"tags":["eu"],"protocol":1,"lastSeen":"2026-01-02T00:00:00Z"},
  {"id":"old-1","name":"Old Build","address":"old.example:27015","gamemode":"freeroam",
   "players":1,"maxPlayers":32,"tags":[],"protocol":2,"lastSeen":"2026-01-02T00:00:00Z"},
  {"id":"na-1","name":"Full","address":"na.example:27015","gamemode":"racing",
   "players":32,"maxPlayers":32,"tags":["na"],"protocol":1,"lastSeen":"2026-01-02T00:00:00Z"}
])";

MasterClient make_master(bool reachable = true) {
  MasterClient::HttpGet get = [reachable](const std::string& url) -> std::optional<std::string> {
    if (!reachable)
      return std::nullopt;
    if (url.find("/servers") != std::string::npos)
      return std::string(kServers);
    return std::nullopt;
  };
  MasterClient::HttpPost post = [](const std::string&, const std::string&) { return true; };
  return MasterClient("http://master.test", std::move(get), std::move(post));
}

}  // namespace

KCD2S_TEST(frontend_menu, refresh_populates_browsing) {
  FrontendMenu menu(make_master(), /*own_protocol=*/1);
  KCD2S_CHECK(menu.state() == State::Splash);
  KCD2S_CHECK(menu.refresh());
  KCD2S_CHECK(menu.state() == State::Browsing);
  KCD2S_CHECK_EQ(menu.browser().total(), std::size_t{3});
}

KCD2S_TEST(frontend_menu, refresh_failure_is_failed_state) {
  FrontendMenu menu(make_master(/*reachable=*/false), 1);
  KCD2S_CHECK(!menu.refresh());
  KCD2S_CHECK(menu.state() == State::Failed);
  KCD2S_CHECK(!menu.error().empty());
}

KCD2S_TEST(frontend_menu, connect_compatible_server) {
  FrontendMenu menu(make_master(), 1);
  menu.refresh();
  KCD2S_CHECK(menu.connect("eu-1"));
  KCD2S_CHECK(menu.state() == State::Connecting);
  KCD2S_CHECK_EQ(menu.connect_target(), std::string("eu.example:27015"));
  KCD2S_CHECK_EQ(menu.connect_id(), std::string("eu-1"));

  menu.on_connect_result(true);
  KCD2S_CHECK(menu.state() == State::InGame);
}

KCD2S_TEST(frontend_menu, connect_incompatible_protocol_rejected) {
  FrontendMenu menu(make_master(), 1);
  menu.refresh();
  KCD2S_CHECK(!menu.connect("old-1"));  // server protocol 2 != client 1
  KCD2S_CHECK(menu.state() == State::Failed);
  KCD2S_CHECK(menu.error().find("incompatible") != std::string::npos);
  // Recover to the browser.
  menu.back();
  KCD2S_CHECK(menu.state() == State::Browsing);
}

KCD2S_TEST(frontend_menu, connect_unknown_id_rejected) {
  FrontendMenu menu(make_master(), 1);
  menu.refresh();
  KCD2S_CHECK(!menu.connect("ghost"));
  KCD2S_CHECK(menu.state() == State::Failed);
}

KCD2S_TEST(frontend_menu, connect_failure_then_back) {
  FrontendMenu menu(make_master(), 1);
  menu.refresh();
  menu.connect("eu-1");
  menu.on_connect_result(false);
  KCD2S_CHECK(menu.state() == State::Failed);
  KCD2S_CHECK(!menu.error().empty());
  menu.back();
  KCD2S_CHECK(menu.state() == State::Browsing);
}

KCD2S_TEST(frontend_menu, direct_connect_skips_directory) {
  FrontendMenu menu(make_master(), 1);
  // Direct connect works even from Splash (no refresh needed).
  menu.direct_connect("192.168.1.10:27015");
  KCD2S_CHECK(menu.state() == State::Connecting);
  KCD2S_CHECK_EQ(menu.connect_target(), std::string("192.168.1.10:27015"));
  KCD2S_CHECK(menu.connect_id().empty());
  menu.on_connect_result(true);
  KCD2S_CHECK(menu.state() == State::InGame);
}

KCD2S_TEST(frontend_menu, zero_protocol_client_ignores_compat) {
  FrontendMenu menu(make_master(), /*own_protocol=*/0);  // "don't care"
  menu.refresh();
  KCD2S_CHECK(menu.connect("old-1"));  // protocol 2 accepted when client is 0
  KCD2S_CHECK(menu.state() == State::Connecting);
}
