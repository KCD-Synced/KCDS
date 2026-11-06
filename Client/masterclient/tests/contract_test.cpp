// Cross-language wire-format contract (C++ side). Parses the SAME shared fixture
// Backend/platform/testdata/serverlist.json that the Go master's contract_test.go pins its Entry
// encoding to, so the client parser and the server encoder cannot drift apart.
#include <fstream>
#include <sstream>
#include <string>

#include "kcd2s/platform/server_browser.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::platform::parse_server_list;
using kcd2s::platform::ServerBrowser;
using kcd2s::platform::ServerEntry;

#ifndef KCD2S_MASTER_FIXTURE
#define KCD2S_MASTER_FIXTURE ""
#endif

static std::string read_fixture() {
  std::ifstream f(KCD2S_MASTER_FIXTURE, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

KCD2S_TEST(master_contract, fixture_parses_to_expected_entries) {
  const std::string json = read_fixture();
  KCD2S_CHECK(!json.empty());  // fixture found (path wired by CMake)

  auto parsed = parse_server_list(json);
  KCD2S_CHECK(parsed.has_value());
  if (!parsed)
    return;
  const auto& v = *parsed;
  KCD2S_CHECK_EQ(v.size(), std::size_t{3});

  KCD2S_CHECK_EQ(v[0].id, std::string("eu-1"));
  KCD2S_CHECK_EQ(v[0].name, std::string("Rattay Freeroam"));
  KCD2S_CHECK_EQ(v[0].address, std::string("play.example.com:27015"));
  KCD2S_CHECK_EQ(v[0].gamemode, std::string("freeroam"));
  KCD2S_CHECK_EQ(v[0].map, std::string("trosecko"));
  KCD2S_CHECK_EQ(v[0].version, std::string("0.1.1"));
  KCD2S_CHECK_EQ(v[0].image, std::string("https://cdn.example.com/art/rattay.jpg"));
  KCD2S_CHECK(!v[0].queue);
  KCD2S_CHECK(v[1].queue);
  KCD2S_CHECK_EQ(v[0].players, 12);
  KCD2S_CHECK_EQ(v[0].maxPlayers, 32);
  KCD2S_CHECK_EQ(v[0].protocol, 1);
  KCD2S_CHECK_EQ(v[0].tags.size(), std::size_t{2});
  KCD2S_CHECK_EQ(v[0].tags[0], std::string("eu"));
  KCD2S_CHECK_EQ(v[0].tags[1], std::string("pvp"));

  KCD2S_CHECK_EQ(v[1].players, 32);  // full
  KCD2S_CHECK_EQ(v[2].tags.size(), std::size_t{0});
  KCD2S_CHECK_EQ(v[2].protocol, 2);
  KCD2S_CHECK(!v[0].password);
  KCD2S_CHECK(v[2].password);
}

KCD2S_TEST(master_contract, browser_over_fixture) {
  auto parsed = parse_server_list(read_fixture());
  KCD2S_CHECK(parsed.has_value());
  if (!parsed)
    return;

  // A protocol-1 client hiding full servers should see only eu-1 (na-1 is full, eu-2 is protocol
  // 2).
  ServerBrowser b;
  b.set_servers(*parsed);
  b.set_protocol(1);
  b.set_hide_full(true);
  auto view = b.view();
  KCD2S_CHECK_EQ(view.size(), std::size_t{1});
  if (!view.empty())
    KCD2S_CHECK_EQ(view[0].id, std::string("eu-1"));
}
