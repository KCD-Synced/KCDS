#include "kcd2s/platform/frontend_menu.hpp"

namespace kcd2s::platform {

void FrontendMenu::fail(std::string reason) {
  state_ = State::Failed;
  error_ = std::move(reason);
}

bool FrontendMenu::refresh() {
  auto servers = master_.fetch_servers();
  if (!servers) {
    fail("could not reach the master server");
    return false;
  }
  browser_.set_servers(std::move(*servers));
  error_.clear();
  state_ = State::Browsing;
  return true;
}

bool FrontendMenu::connect(const std::string& id) {
  // Find the server in the full directory (not just the filtered view) by id.
  const ServerEntry* found = nullptr;
  for (const ServerEntry& e : browser_.all())
    if (e.id == id) {
      found = &e;
      break;
    }
  if (!found) {
    fail("server '" + id + "' is no longer listed");
    return false;
  }
  // Protocol-compatibility gate (0 = client does not care).
  if (own_protocol_ != 0 && found->protocol != 0 && found->protocol != own_protocol_) {
    fail("incompatible protocol: server is v" + std::to_string(found->protocol) + ", client is v" +
         std::to_string(own_protocol_));
    return false;
  }
  connect_target_ = found->address;
  connect_id_ = found->id;
  error_.clear();
  state_ = State::Connecting;
  return true;
}

void FrontendMenu::direct_connect(const std::string& address) {
  connect_target_ = address;
  connect_id_.clear();
  error_.clear();
  state_ = State::Connecting;
}

void FrontendMenu::on_connect_result(bool ok) {
  if (state_ != State::Connecting)
    return;
  if (ok) {
    state_ = State::InGame;
    error_.clear();
  } else {
    fail("failed to connect to " + connect_target_);
  }
}

void FrontendMenu::leave_world() {
  if (state_ == State::InGame) {
    state_ = State::Browsing;
  }
}

void FrontendMenu::back() {
  if (state_ == State::Connecting || state_ == State::Failed) {
    error_.clear();
    state_ = State::Browsing;
  }
}

}  // namespace kcd2s::platform
