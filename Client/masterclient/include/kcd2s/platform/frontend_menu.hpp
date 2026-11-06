// FrontendMenu — the brain of our custom start menu that replaces KCD2's stock main menu
// It is a headless state machine over the master-server directory:
// it fetches the server list, drives the server browser (filter/sort), gates a connect on KSTP
// protocol compatibility, and emits a connect intent (address) the client acts on. The actual
// rendering (our Scaleform/CEF menu) and the hook that shows it instead of the stock menu are
// PREPARE-ONLY (engine-gated) — this class is the logic they drive, fully
// testable without an engine.
//
//   Splash --refresh()--> Browsing --connect(id)/direct_connect(addr)--> Connecting
//     Connecting --on_connect_result(true)--> InGame
//     Connecting --on_connect_result(false)/connect-gate-fail--> Failed
//     Failed/Connecting --back()--> Browsing
#ifndef KCD2S_PLATFORM_FRONTEND_MENU_HPP
#define KCD2S_PLATFORM_FRONTEND_MENU_HPP

#include <string>

#include "kcd2s/platform/master_client.hpp"
#include "kcd2s/platform/server_browser.hpp"

namespace kcd2s::platform {

class FrontendMenu {
 public:
  enum class State { Splash, Browsing, Connecting, InGame, Failed };

  // `own_protocol` is this client's KSTP protocol version (the connect-compat gate). The menu owns
  // a MasterClient for the directory and a ServerBrowser for the view.
  FrontendMenu(MasterClient master, int own_protocol)
      : master_(std::move(master)), own_protocol_(own_protocol) {
  }

  State state() const {
    return state_;
  }
  const std::string& error() const {
    return error_;
  }  // reason when state()==Failed

  // Fetch the directory and enter Browsing. Returns false (and sets Failed+error) on a transport or
  // parse failure. Usable from Splash, Browsing, Failed, or InGame (e.g. after disconnect).
  bool refresh();

  // The browser view the menu renders (filter/sort configured via browser()).
  ServerBrowser& browser() {
    return browser_;
  }
  const ServerBrowser& browser() const {
    return browser_;
  }

  // Connect to a listed server by id. Gates on protocol compatibility: an incompatible server -> no
  // connect, state Failed with a reason. On success -> Connecting, and
  // connect_target()/connect_id() hold the address/id the client should dial.
  bool connect(const std::string& id);

  // Direct connect to an address (skips the directory; no protocol info to check up front).
  void direct_connect(const std::string& address);

  // The client reports the transport outcome of the Connecting attempt.
  void on_connect_result(bool ok);

  // Return to the browser (from Connecting or Failed). No-op otherwise.
  void back();
  // The player left the server's world back to the menu (from InGame). No-op otherwise.
  void leave_world();

  const std::string& connect_target() const {
    return connect_target_;
  }
  const std::string& connect_id() const {
    return connect_id_;
  }

 private:
  void fail(std::string reason);

  MasterClient master_;
  int own_protocol_;
  ServerBrowser browser_;
  State state_ = State::Splash;
  std::string error_;
  std::string connect_target_;  // address being dialled
  std::string connect_id_;      // directory id (empty for a direct connect)
};

}  // namespace kcd2s::platform

#endif  // KCD2S_PLATFORM_FRONTEND_MENU_HPP
