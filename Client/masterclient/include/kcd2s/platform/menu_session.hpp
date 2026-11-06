// MenuSession — the game side of the in-game server browser page (Client/ui/menu/index.html).
//
// The page and the game exchange small JSON messages. The page sends
//   {"type":"ready"}                      page loaded: send who we are and the list
//   {"type":"refresh"}                    fetch the list again
//   {"type":"connect","id":"<id>"}        join a listed server ("queue":true after ConfirmQueue)
//   {"type":"direct","address":"h:p"}     join an address typed by the player
//                                         (either with "password":"..." once the server asked)
//   {"type":"cancel"}                     abandon the join (or leave the queue)
//   {"type":"update"}                     the player chose to update (UpdateRequired)
//   {"type":"quit"}                       the player chose to quit the game
//   {"type":"dismiss"}                    the player closed a popup shown over the world
//   {"type":"tool","tool":"studio"}       ANIM / ASSETS: the game starts again in the Asset/
//                                         Animation Studio; "architect": in World
//                                         Architect. Only on the list, not during a join.
//   {"type":"verifyDiscord"}              the player chose to verify their Discord account
// and receives
//   {"type":"self","protocol":N,"version":"x.y.z","gameVersion":"a.b.c"}
//   {"type":"servers","servers":[...],"error":"..."}       error only when the master failed
//   {"type":"state","state":"connecting","step":N,"server":{...}}
//   {"type":"state","state":"connecting","step":2,"server":{...},
//    "progress":{"resourcesDone":N,"resourcesTotal":M,"bytesDone":B,"bytesTotal":T}}
//   {"type":"state","state":"failed","kind":"...","step":N,"reason":"...","code":"...",
//    "message":"..."}                     message: the server's own reason, when it gave one
//   {"type":"state","state":"update","yours":"...","required":"...","newer":bool,...}
//   {"type":"state","state":"queue","place":N,"waited":S,"fields":[{"label","value"}],...}
//   {"type":"state","state":"ingame"|"disconnected",...}
//   {"type":"discord","state":"waiting"|"verified"|"failed","message":"..."}  Verify Discord
//
// Everything the page shows is data rendered as text: server names, reasons and queue fields
// never carry markup. MenuSession performs no I/O of its own except the master request inside
// the injected MasterClient, so it runs on a thread that may block. It is not thread-safe.
#ifndef KCD2S_PLATFORM_MENU_SESSION_HPP
#define KCD2S_PLATFORM_MENU_SESSION_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/platform/frontend_menu.hpp"
#include "kcd2s/platform/master_client.hpp"

namespace kcd2s::platform {

// The steps of a join, as the connecting popup lists them.
enum class JoinStep {
  Reaching = 0,      // waiting for the server's first answer
  Admission = 1,     // handshake: the server checks this game's KCDS build
  Requirements = 2,  // the server's requirements: licence, password, bans, a verified Discord
                     // account, its playerConnecting handlers (whitelists)
  Resources = 3,     // the server's world and client resources arrive
  World = 4,         // the game loads the world
};

// Why a join failed.
enum class JoinFailure {
  Timeout,           // no answer in time
  Full,              // the server refused: every place is taken
  Rejected,          // a playerConnecting handler turned the player away
  AdmissionTimeout,  // playerConnecting did not decide in time
  Address,           // the address could not be resolved
  Internal,          // the client could not start the connection
  Banned,            // the Master's central ban list refused a licence pass
  Password,          // the server asks for a password: none was given, or a wrong one
  Level,             // the server runs a level this game does not have (a DLC level)
  Licence,           // this copy of the game could not be proven an original one
  GameVersion,       // this release does not support the game's build: a game function it
                     // needs was not found
  ServerBanned,      // this server's own bans name the player (Player.ban, Bans, KCDS-Admin)
  Discord,           // the server lets in only verified Discord accounts
};

// The longest server password the page may send (the server's KCDS_SERVER_PASSWORD limit).
inline constexpr std::size_t kMaxServerPasswordBytes = 64;

// One line of server-provided queue information. Plain text only.
struct QueueField {
  std::string label;
  std::string value;
};

inline constexpr std::size_t kMaxQueueFields = 3;
inline constexpr std::size_t kMaxQueueLabelBytes = 32;
inline constexpr std::size_t kMaxQueueValueBytes = 96;
// A refusing server's own reason (the transport's kMaxRefusalTextBytes), or the
// Master's word on a central ban.
inline constexpr std::size_t kMaxServerMessageBytes = 512;

// At most kMaxQueueFields fields, each label/value cut to its byte limit on a UTF-8 boundary,
// with control characters removed and empty fields dropped.
std::vector<QueueField> sanitize_queue_fields(const std::vector<QueueField>& fields);

class MenuSession {
 public:
  // client_version is our release, game_version the game's (shown in the footer, may be empty).
  MenuSession(MasterClient master, int own_protocol, std::string client_version,
              std::string game_version = {});

  // Handles one message from the page and returns the messages to post back, in order. Malformed
  // or unknown messages are ignored.
  std::vector<std::string> handle_page_message(const std::string& json);

  // The address the player chose, once per join request. The client dials it and reports the
  // progress with join_progress() and the outcome with joined() or join_failed().
  std::optional<std::string> take_connect_target();
  // A join link (kcds): the address, or else the Master listing id, joined once
  // when the page is first ready, as if the player had chosen it there.
  void set_join_link(std::string address, std::string server) {
    link_address_ = std::move(address);
    link_server_ = std::move(server);
  }
  // The password the player typed for that join, or empty. Kept for a reconnect.
  const std::string& join_password() const {
    return password_;
  }
  // The server of the last join request (its name; only name and address for a direct join).
  const ServerEntry& target() const {
    return target_;
  }

  // True once after the player cancelled the join in progress; the client abandons the attempt.
  bool take_cancel();
  // True once after the player chose to update or to quit the game.
  bool take_update();
  bool take_quit();
  bool take_dismiss();
  // True once after the player pressed Verify Discord.
  bool take_verify_discord();
  // How the Discord verification goes, for the page: "waiting", "verified" or "failed" with the
  // player's message.
  static std::string discord_message(const char* state, const std::string& message = {});

  // The tool the player chose on the list, returned once.
  enum class Tool { None, Studio, Architect };
  Tool take_tool();

  // The player chose to leave the server's world: back to the list, or back and straight into the
  // same server again. Returned once per choice.
  enum class Leave { None, BackToList, Reconnect };
  Leave take_leave();
  // The world is gone and the menu shows again: the list returns, refreshed.
  std::vector<std::string> returned_to_list();
  // Joins the server of the last join again (RECONNECT after the world was left). Empty when
  // there is no previous server.
  std::vector<std::string> reconnect();

  std::vector<std::string> join_progress(JoinStep step);
  // Client resource download progress while the join is at JoinStep::Resources. Counts are
  // clamped to be non-negative with done <= total.
  std::vector<std::string> resource_progress(std::int64_t resources_done,
                                             std::int64_t resources_total, std::int64_t bytes_done,
                                             std::int64_t bytes_total);
  std::vector<std::string> joined();
  // A failed join returns to the list. A timeout against a server that announced another
  // version is reported as an update instead, because a different build is the likely cause.
  // server_message is the refusing server's own reason (connection.reject), shown as plain text.
  std::vector<std::string> join_failed(JoinFailure kind, JoinStep step, const std::string& detail,
                                       const std::string& server_message = {});
  // The server refused the join because it runs another KCDS version: the update
  // message with the version it named (empty when its text named none), for every way of joining.
  std::vector<std::string> version_refused(const std::string& server_version);
  // The session ended after the player joined; reason is shown as plain text. cause names why, for
  // the dialog's title: "lost" (no answer), "closed" (the server shut down), "kicked", "left" (a
  // script let the player go) or "banned" (no reconnect then).
  std::vector<std::string> disconnected(const std::string& reason,
                                        const std::string& cause = "lost");
  // Queue position for a queue-capable server (backend).
  std::vector<std::string> queue_update(int place, int waited_seconds,
                                        const std::vector<QueueField>& fields);

  FrontendMenu::State state() const {
    return menu_.state();
  }

  // The transport's own description of the last failed master request (e.g. "send failed
  // (error 12029)"), appended to the unavailable-list error the page shows under "details".
  void set_error_detail(std::function<std::string()> detail) {
    error_detail_ = std::move(detail);
  }

 private:
  std::string self_message() const;
  std::string servers_message(const std::string& error) const;
  std::string state_message(const char* state, const std::string& extra) const;
  // server_version, when given, is the version the server named itself; otherwise the listed one.
  std::string update_message(const std::string& server_version = {}) const;
  std::vector<std::string> refresh();

  FrontendMenu menu_;
  int own_protocol_;
  std::string client_version_;
  std::string game_version_;
  std::optional<std::string> pending_target_;
  std::string password_;
  std::string link_address_;
  std::string link_server_;
  ServerEntry target_;  // the server being joined (only name/address for a direct join)
  bool cancel_ = false;
  bool update_ = false;
  bool quit_ = false;
  bool dismiss_ = false;
  Tool tool_ = Tool::None;
  bool verify_discord_ = false;
  Leave leave_ = Leave::None;
  std::function<std::string()> error_detail_;
};

}  // namespace kcd2s::platform

#endif  // KCD2S_PLATFORM_MENU_SESSION_HPP
