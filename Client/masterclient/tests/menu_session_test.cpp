// MenuSession: the page protocol of the in-game server browser over a fake master.
#include "kcd2s/platform/menu_session.hpp"

#include <optional>
#include <string>
#include <vector>

#include "kcd2s/platform/master_client.hpp"
#include "kcd2s/platform/server_browser.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::platform::FrontendMenu;
using kcd2s::platform::JoinFailure;
using kcd2s::platform::JoinStep;
using kcd2s::platform::MasterClient;
using kcd2s::platform::MenuSession;
using kcd2s::platform::QueueField;

namespace {

const char* kServers = R"([
  {"id":"kit","name":"Kit & Co","address":"127.0.0.1:7796","gamemode":"",
   "map":"trosecko","players":1,"maxPlayers":32,"version":"0.1.1","tags":[],"protocol":2,
   "lastSeen":"2026-10-13T00:00:00Z","image":"https://cdn.example.com/kit.jpg","queue":false},
  {"id":"old","name":"Old","address":"old.example:7777","gamemode":"","map":"trosecko",
   "players":0,"maxPlayers":8,"version":"0.1.0","tags":[],"protocol":1,
   "lastSeen":"2026-10-13T00:00:00Z"},
  {"id":"next","name":"Next","address":"next.example:7777","gamemode":"","map":"trosecko",
   "players":0,"maxPlayers":8,"version":"0.1.2","tags":[],"protocol":2,
   "lastSeen":"2026-10-13T00:00:00Z"}
])";

MasterClient make_master(bool* reachable, int* requests = nullptr) {
  MasterClient::HttpGet get = [reachable,
                               requests](const std::string& url) -> std::optional<std::string> {
    if (requests)
      ++*requests;
    if (!*reachable || url != "https://master.test/servers")
      return std::nullopt;
    return std::string(kServers);
  };
  return MasterClient("https://master.test", std::move(get), nullptr);
}

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

MenuSession ready_session(bool* reachable) {
  MenuSession session(make_master(reachable), 2, "0.1.1", "1.5.6");
  session.handle_page_message(R"({"type":"ready"})");
  return session;
}

}  // namespace

KCD2S_TEST(menu_session, ready_sends_self_then_live_list) {
  bool reachable = true;
  MenuSession session(make_master(&reachable), 2, "0.1.1", "1.5.6");
  auto out = session.handle_page_message(R"({"type":"ready"})");
  KCD2S_CHECK_EQ(out.size(), std::size_t{2});
  if (out.size() != 2)
    return;
  KCD2S_CHECK_EQ(
      out[0],
      std::string(R"({"type":"self","protocol":2,"version":"0.1.1","gameVersion":"1.5.6"})"));
  KCD2S_CHECK(contains(out[1], R"("type":"servers")"));
  KCD2S_CHECK(contains(out[1], R"("name":"Kit & Co")"));
  KCD2S_CHECK(contains(out[1], R"("image":"https://cdn.example.com/kit.jpg")"));
  KCD2S_CHECK(contains(out[1], R"("queue":false)"));
  KCD2S_CHECK(!contains(out[1], R"("error")"));
  KCD2S_CHECK(session.state() == FrontendMenu::State::Browsing);
}

KCD2S_TEST(menu_session, unreachable_master_gives_empty_list_with_reason) {
  bool reachable = false;
  MenuSession session(make_master(&reachable), 2, "0.1.1");
  auto out = session.handle_page_message(R"({"type":"refresh"})");
  KCD2S_CHECK_EQ(out.size(), std::size_t{1});
  if (out.empty())
    return;
  KCD2S_CHECK(contains(out[0], R"("servers":[])"));
  KCD2S_CHECK(contains(out[0], R"("error":"could not reach the master server")"));
  KCD2S_CHECK(session.state() == FrontendMenu::State::Browsing);

  session.set_error_detail([] { return std::string("send failed (error 12029)"); });
  out = session.handle_page_message(R"({"type":"refresh"})");
  KCD2S_CHECK(!out.empty() && contains(out[0], "master server: send failed (error 12029)"));

  reachable = true;
  out = session.handle_page_message(R"({"type":"refresh"})");
  KCD2S_CHECK(!out.empty() && contains(out[0], "Kit & Co") && !contains(out[0], "error"));
}

KCD2S_TEST(menu_session, join_reports_steps_once_then_ingame) {
  bool reachable = true;
  MenuSession session = ready_session(&reachable);
  auto out = session.handle_page_message(R"({"type":"connect","id":"kit"})");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"connecting","server":{)"));
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("step":0)"));
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("players":1,"maxPlayers":32)"));
  KCD2S_CHECK_EQ(session.take_connect_target().value_or(""), std::string("127.0.0.1:7796"));
  KCD2S_CHECK(!session.take_connect_target().has_value());
  KCD2S_CHECK(session.handle_page_message(R"({"type":"connect","id":"next"})").empty());

  out = session.join_progress(JoinStep::Resources);
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("step":3)"));
  out = session.resource_progress(3, 5, 19293798, 31037849);
  const std::string progress =
      R"("step":3,"progress":{"resourcesDone":3,"resourcesTotal":5,"bytesDone":19293798,)"
      R"("bytesTotal":31037849})";
  KCD2S_CHECK(!out.empty() && contains(out[0], progress));
  // Out-of-range counts are clamped, never passed through.
  out = session.resource_progress(9, 5, -4, 10);
  const std::string clamped =
      R"("progress":{"resourcesDone":5,"resourcesTotal":5,"bytesDone":0,"bytesTotal":10})";
  KCD2S_CHECK(!out.empty() && contains(out[0], clamped));
  out = session.joined();
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"ingame")"));
  KCD2S_CHECK(session.state() == FrontendMenu::State::InGame);
  KCD2S_CHECK(session.join_progress(JoinStep::World).empty());
  KCD2S_CHECK(session.resource_progress(1, 1, 1, 1).empty());  // only while connecting
}

KCD2S_TEST(menu_session, failures_carry_kind_reason_and_code) {
  bool reachable = true;
  MenuSession session = ready_session(&reachable);
  session.handle_page_message(R"({"type":"connect","id":"kit"})");
  auto out = session.join_failed(JoinFailure::Full, JoinStep::Admission, "");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"failed")"));
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("kind":"full","step":1)"));
  KCD2S_CHECK(!out.empty() &&
              contains(out[0], R"("code":"127.0.0.1:7796 · refused: server full")"));
  KCD2S_CHECK(session.state() == FrontendMenu::State::Browsing);

  session.handle_page_message(R"({"type":"connect","id":"kit"})");
  out = session.join_failed(JoinFailure::Rejected, JoinStep::Admission, "whitelist");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("kind":"rejected")"));
  KCD2S_CHECK(!out.empty() && contains(out[0], "playerConnecting · whitelist"));
  KCD2S_CHECK(!out.empty() && !contains(out[0], R"("message")"));  // no server reason given

  // The server's own reason travels as a JSON string: quotes escaped, control bytes removed,
  // markup left as text for the page to show verbatim.
  session.handle_page_message(R"({"type":"connect","id":"kit"})");
  out = session.join_failed(JoinFailure::Rejected, JoinStep::Admission, "",
                            "Whitelist only: \"apply\"\x01 <b>now</b>");
  const std::string escaped = R"("message":"Whitelist only: \"apply\" <b>now</b>")";
  KCD2S_CHECK(!out.empty() && contains(out[0], escaped));
  session.handle_page_message(R"({"type":"connect","id":"kit"})");
  out = session.join_failed(JoinFailure::Rejected, JoinStep::Admission, "", std::string(600, 'a'));
  KCD2S_CHECK(!out.empty() && contains(out[0], std::string(512, 'a') + "\""));
  KCD2S_CHECK(!out.empty() && !contains(out[0], std::string(513, 'a')));

  // A server password: the page's password goes with the join, its own failure kind.
  session.handle_page_message(R"({"type":"direct","address":"127.0.0.1:7841","password":"pw"})");
  KCD2S_CHECK(session.join_password() == "pw");
  out = session.join_failed(JoinFailure::Password, JoinStep::Admission, "", "Wrong password.");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("kind":"password")"));
  KCD2S_CHECK(!out.empty() && contains(out[0], "refused: password"));
  session.handle_page_message(R"({"type":"direct","address":"127.0.0.1:7841"})");
  KCD2S_CHECK(session.join_password().empty());
  session.join_failed(JoinFailure::Timeout, JoinStep::Reaching, "");
  const std::string long_password(65, 'p');
  session.handle_page_message(R"({"type":"connect","id":"kit","password":")" + long_password +
                              R"("})");
  KCD2S_CHECK(session.join_password().empty());
  session.join_failed(JoinFailure::Timeout, JoinStep::Reaching, "");

  // A server asking for a verified Discord account: its own kind, and the
  // page's Verify Discord comes back once.
  session.handle_page_message(R"({"type":"connect","id":"kit"})");
  out = session.join_failed(JoinFailure::Discord, JoinStep::Requirements, "",
                            "This server asks for a verified Discord account.");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("kind":"discord","step":2)"));
  KCD2S_CHECK(!out.empty() && contains(out[0], "refused: Discord not verified"));
  session.handle_page_message(R"({"type":"verifyDiscord"})");
  KCD2S_CHECK(session.take_verify_discord());
  KCD2S_CHECK(!session.take_verify_discord());
  // Quotes escaped and control bytes removed (a raw string is kept out of the check macro).
  const std::string discord_failed =
      R"({"type":"discord","state":"failed","message":"No \"answer\""})";
  KCD2S_CHECK(MenuSession::discord_message("failed", "No \"answer\"\x01") == discord_failed);

  // A copy that could not be proven an original one: its own kind and code.
  session.handle_page_message(R"({"type":"connect","id":"kit"})");
  out = session.join_failed(JoinFailure::Licence, JoinStep::Admission, "", "GOG is not supported");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("kind":"licence")"));
  KCD2S_CHECK(!out.empty() && contains(out[0], "licence not proven"));

  // A game build this release does not support: its own kind and code.
  session.handle_page_message(R"({"type":"connect","id":"kit"})");
  out = session.join_failed(JoinFailure::GameVersion, JoinStep::Reaching, "");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("kind":"gameVersion")"));
  KCD2S_CHECK(!out.empty() && contains(out[0], "unsupported game version"));

  // A central ban: its own kind and code, the Master's words as the message.
  session.handle_page_message(R"({"type":"connect","id":"kit"})");
  out = session.join_failed(JoinFailure::Banned, JoinStep::Admission, "", "Reason: Cheating.");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("kind":"banned")"));
  KCD2S_CHECK(!out.empty() && contains(out[0], "refused: KCDS central ban"));
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("message":"Reason: Cheating.")"));

  session.handle_page_message(R"({"type":"connect","id":"kit"})");
  out = session.join_failed(JoinFailure::Timeout, JoinStep::Reaching, "");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("kind":"timeout","step":0)"));
  KCD2S_CHECK(session.join_failed(JoinFailure::Timeout, JoinStep::Reaching, "").empty());
}

KCD2S_TEST(menu_session, other_protocol_or_silent_other_version_asks_for_update) {
  bool reachable = true;
  MenuSession session = ready_session(&reachable);
  auto out = session.handle_page_message(R"({"type":"connect","id":"old"})");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"update")"));
  KCD2S_CHECK(!out.empty() &&
              contains(out[0], R"("yours":"0.1.1","required":"0.1.0","newer":false)"));
  KCD2S_CHECK(!session.take_connect_target().has_value());

  // Same protocol, newer version: the join is tried; a timeout then reads as an update.
  out = session.handle_page_message(R"({"type":"connect","id":"next"})");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"connecting")"));
  out = session.join_failed(JoinFailure::Timeout, JoinStep::Reaching, "");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"update")"));
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("required":"0.1.2","newer":true)"));

  session.handle_page_message(R"({"type":"update"})");
  KCD2S_CHECK(session.take_update());
  KCD2S_CHECK(!session.take_update());
}

// A direct join refused by a server of another version: the update message names the
// version the server gave, older or newer than ours.
KCD2S_TEST(menu_session, direct_join_refused_by_other_version_asks_for_update) {
  bool reachable = true;
  MenuSession session(make_master(&reachable), 2, "0.1.1");
  session.handle_page_message(R"({"type":"direct","address":"play.example.com:7777"})");
  auto out = session.version_refused("0.1.0");
  KCD2S_CHECK(!out.empty() &&
              contains(out[0], R"("yours":"0.1.1","required":"0.1.0","newer":false)"));
  KCD2S_CHECK(session.version_refused("0.1.0").empty());

  session.handle_page_message(R"({"type":"direct","address":"play.example.com:7777"})");
  out = session.version_refused("0.1.2");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("required":"0.1.2","newer":true)"));

  // A refusal whose text named no version still says another version is needed.
  session.handle_page_message(R"({"type":"direct","address":"play.example.com:7777"})");
  out = session.version_refused("");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"update")"));
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("required":"another version")"));
}

KCD2S_TEST(menu_session, direct_connect_validates_address) {
  bool reachable = true;
  MenuSession session(make_master(&reachable), 2, "0.1.1");
  auto out = session.handle_page_message(R"({"type":"direct","address":"not an address"})");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("kind":"address")"));
  KCD2S_CHECK(!session.take_connect_target().has_value());

  out = session.handle_page_message(R"({"type":"direct","address":"play.example.com:7777"})");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"connecting")"));
  KCD2S_CHECK_EQ(session.take_connect_target().value_or(""), std::string("play.example.com:7777"));
}

// A host typed without a port, a DNS name or an IP, joins the default game port.
KCD2S_TEST(menu_session, direct_connect_without_port_uses_default_port) {
  bool reachable = true;
  MenuSession session(make_master(&reachable), 2, "0.1.1");
  auto out = session.handle_page_message(R"({"type":"direct","address":" game.example.com "})");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"connecting")"));
  KCD2S_CHECK_EQ(session.take_connect_target().value_or(""), std::string("game.example.com:7777"));
  session.handle_page_message(R"({"type":"cancel"})");

  out = session.handle_page_message(R"({"type":"direct","address":"192.168.1.10"})");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"connecting")"));
  KCD2S_CHECK_EQ(session.take_connect_target().value_or(""), std::string("192.168.1.10:7777"));
  session.handle_page_message(R"({"type":"cancel"})");

  // A port the player typed stays; a broken one is still refused.
  out = session.handle_page_message(R"({"type":"direct","address":"game.example.com:7800"})");
  KCD2S_CHECK_EQ(session.take_connect_target().value_or(""), std::string("game.example.com:7800"));
  session.handle_page_message(R"({"type":"cancel"})");

  out = session.handle_page_message(R"({"type":"direct","address":"game.example.com:"})");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("kind":"address")"));
  KCD2S_CHECK(!session.take_connect_target().has_value());
}

// The gear opens the game's settings from the list, once per press, never during a join.
KCD2S_TEST(menu_session, settings_from_the_list_only) {
  bool reachable = true;
  MenuSession session(make_master(&reachable), 2, "0.1.1");
  KCD2S_CHECK(session.handle_page_message(R"({"type":"settings"})").empty());
  KCD2S_CHECK(session.take_settings());
  KCD2S_CHECK(!session.take_settings());

  session.handle_page_message(R"({"type":"direct","address":"play.example.com"})");
  session.handle_page_message(R"({"type":"settings"})");
  KCD2S_CHECK(!session.take_settings());
}

// The logs button opens the logs folder from the list, once per press, never during a join.
KCD2S_TEST(menu_session, logs_folder_from_the_list_only) {
  bool reachable = true;
  MenuSession session(make_master(&reachable), 2, "0.1.1");
  KCD2S_CHECK(session.handle_page_message(R"({"type":"logs"})").empty());
  KCD2S_CHECK(session.take_logs());
  KCD2S_CHECK(!session.take_logs());

  session.handle_page_message(R"({"type":"direct","address":"play.example.com"})");
  session.handle_page_message(R"({"type":"logs"})");
  KCD2S_CHECK(!session.take_logs());
}

KCD2S_TEST(menu_session, cancel_and_quit) {
  bool reachable = true;
  MenuSession session(make_master(&reachable), 2, "0.1.1");
  session.handle_page_message(R"({"type":"direct","address":"127.0.0.1:7796"})");
  KCD2S_CHECK(!session.take_cancel());
  session.handle_page_message(R"({"type":"cancel"})");
  KCD2S_CHECK(session.take_cancel());
  KCD2S_CHECK(!session.take_cancel());
  KCD2S_CHECK(session.state() == FrontendMenu::State::Browsing);
  session.handle_page_message(R"({"type":"quit"})");
  KCD2S_CHECK(session.take_quit());
}

KCD2S_TEST(menu_session, tools_on_the_list_only) {
  bool reachable = true;
  MenuSession session(make_master(&reachable), 2, "0.1.1");
  KCD2S_CHECK(session.take_tool() == MenuSession::Tool::None);
  session.handle_page_message(R"({"type":"tool","tool":"studio"})");
  KCD2S_CHECK(session.take_tool() == MenuSession::Tool::Studio);
  KCD2S_CHECK(session.take_tool() == MenuSession::Tool::None);
  session.handle_page_message(R"({"type":"tool","tool":"architect"})");
  KCD2S_CHECK(session.take_tool() == MenuSession::Tool::Architect);
  session.handle_page_message(R"({"type":"tool","tool":"editor"})");
  KCD2S_CHECK(session.take_tool() == MenuSession::Tool::None);
  // Not while a join is in progress.
  session.handle_page_message(R"({"type":"direct","address":"127.0.0.1:7796"})");
  session.handle_page_message(R"({"type":"tool","tool":"studio"})");
  KCD2S_CHECK(session.take_tool() == MenuSession::Tool::None);
}

KCD2S_TEST(menu_session, disconnect_and_queue_fields_are_plain_bounded_text) {
  bool reachable = true;
  MenuSession session = ready_session(&reachable);
  auto out = session.disconnected("The server stopped\nanswering");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("reason":"The server stoppedanswering")"));
  KCD2S_CHECK(contains(out[0], R"("cause":"lost")"));
  out = session.disconnected("You are banned from this server: Cheating.", "banned");
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("cause":"banned")"));

  std::vector<QueueField> fields = {
      {"Priority", "<b>VIP</b>"},
      {std::string(40, 'L'), std::string(120, 'v')},
      {"", ""},
      {"Third", "3"},
      {"Fourth", "dropped"},
  };
  out = session.queue_update(4, 161, fields);
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("state":"queue")"));
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("place":4,"waited":161)"));
  KCD2S_CHECK(!out.empty() && contains(out[0], R"({"label":"Priority","value":"<b>VIP</b>"})"));
  const std::string long_label = std::string("\"label\":\"") + std::string(32, 'L') + "\"";
  const std::string long_value = std::string(96, 'v') + "\"}";
  KCD2S_CHECK(!out.empty() && contains(out[0], long_label));
  KCD2S_CHECK(!out.empty() && contains(out[0], long_value));
  KCD2S_CHECK(!out.empty() && contains(out[0], R"("label":"Third")"));
  KCD2S_CHECK(!out.empty() && !contains(out[0], "Fourth"));

  const auto utf8 =
      kcd2s::platform::sanitize_queue_fields({{"", std::string(95, 'x') + "\xC3\xA9"}});
  KCD2S_CHECK_EQ(utf8.size(), std::size_t{1});
  KCD2S_CHECK_EQ(utf8.empty() ? 0 : utf8[0].value.size(), std::size_t{95});
}

KCD2S_TEST(menu_session, malformed_messages_are_ignored) {
  bool reachable = true;
  int requests = 0;
  MenuSession session(make_master(&reachable, &requests), 2, "0.1.1");
  KCD2S_CHECK(session.handle_page_message("not json").empty());
  KCD2S_CHECK(session.handle_page_message("[1,2]").empty());
  KCD2S_CHECK(session.handle_page_message(R"({"type":7})").empty());
  KCD2S_CHECK(session.handle_page_message(R"({"type":"unknown"})").empty());
  KCD2S_CHECK(session.handle_page_message(std::string(5000, ' ') + "{}").empty());
  KCD2S_CHECK_EQ(requests, 0);
}

KCD2S_TEST(menu_session, server_list_round_trips_through_encoder) {
  auto parsed = kcd2s::platform::parse_server_list(kServers);
  KCD2S_CHECK(parsed.has_value());
  if (!parsed)
    return;
  auto again = kcd2s::platform::parse_server_list(kcd2s::platform::encode_server_list(*parsed));
  KCD2S_CHECK(again.has_value());
  if (!again)
    return;
  KCD2S_CHECK_EQ(again->size(), std::size_t{3});
  KCD2S_CHECK_EQ((*again)[0].name, std::string("Kit & Co"));
  KCD2S_CHECK_EQ((*again)[0].image, std::string("https://cdn.example.com/kit.jpg"));
  KCD2S_CHECK_EQ((*again)[2].version, std::string("0.1.2"));
}

// After a join the player can leave the world for the list, or leave and join the same server
// again; both are refused outside a world.
KCD2S_TEST(menu_session, leaving_a_world_returns_to_the_list_or_reconnects) {
  bool reachable = true;
  MenuSession session = ready_session(&reachable);
  KCD2S_CHECK(session.handle_page_message(R"({"type":"back"})").empty());
  KCD2S_CHECK(session.take_leave() == MenuSession::Leave::None);

  session.handle_page_message(R"({"type":"connect","id":"kit"})");
  KCD2S_CHECK_EQ(session.take_connect_target().value_or(""), std::string("127.0.0.1:7796"));
  session.joined();
  KCD2S_CHECK(session.state() == FrontendMenu::State::InGame);

  session.handle_page_message(R"({"type":"reconnect"})");
  KCD2S_CHECK(session.take_leave() == MenuSession::Leave::Reconnect);
  KCD2S_CHECK(session.take_leave() == MenuSession::Leave::None);

  auto list = session.returned_to_list();
  KCD2S_CHECK(contains(list.front(), R"("state":"browsing")"));
  KCD2S_CHECK(session.state() == FrontendMenu::State::Browsing);
  auto again = session.reconnect();
  KCD2S_CHECK_EQ(again.size(), std::size_t{1});
  KCD2S_CHECK(contains(again[0], R"("state":"connecting")"));
  KCD2S_CHECK_EQ(session.take_connect_target().value_or(""), std::string("127.0.0.1:7796"));
}

// a kcds:// join link is joined once the page is first ready, by address or by the
// listed server's id, and only once.
KCD2S_TEST(menu_session, join_link_joins_when_the_page_is_ready) {
  bool reachable = true;
  MenuSession by_address(make_master(&reachable), 2, "0.1.1", "1.5.6");
  by_address.set_join_link("play.example.com:7777", "");
  auto out = by_address.handle_page_message(R"({"type":"ready"})");
  KCD2S_CHECK_EQ(out.size(), std::size_t{3});
  KCD2S_CHECK(!out.empty() && contains(out.back(), R"("state":"connecting")"));
  KCD2S_CHECK(!out.empty() && contains(out.back(), R"("address":"play.example.com:7777")"));
  KCD2S_CHECK(by_address.take_connect_target() ==
              std::optional<std::string>("play.example.com:7777"));
  by_address.join_failed(JoinFailure::Timeout, JoinStep::Reaching, "");
  // A second ready (the page reloaded) does not join again.
  out = by_address.handle_page_message(R"({"type":"ready"})");
  KCD2S_CHECK_EQ(out.size(), std::size_t{2});

  MenuSession by_id(make_master(&reachable), 2, "0.1.1", "1.5.6");
  by_id.set_join_link("", "kit");
  out = by_id.handle_page_message(R"({"type":"ready"})");
  KCD2S_CHECK(!out.empty() && contains(out.back(), R"("state":"connecting")"));
  KCD2S_CHECK(!out.empty() && contains(out.back(), R"("id":"kit")"));
  KCD2S_CHECK(by_id.take_connect_target() == std::optional<std::string>("127.0.0.1:7796"));
}
