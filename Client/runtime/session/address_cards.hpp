// The server browser's favourites by address: each server asked for its own card
// (GET http://<address>/kcds/info.json on its game port, web/file_server.cjs) and pinged as the
// listed ones are (a TCP connect), off the browser's thread, a few at once.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/platform/menu_session.hpp"

namespace kcds::runtime::session::address_cards {

// Starts asking `addresses` (host:port) for the round `generation`; a round already asked or being
// asked is not started again.
void start(std::uint64_t generation, std::vector<std::string> addresses);

// The cards of the last round once all are in (one per address, in order; `reached` false for a
// server that did not answer with its card), once.
std::optional<std::vector<kcd2s::platform::MenuSession::AddressCard>> take_results();

}  // namespace kcds::runtime::session::address_cards
