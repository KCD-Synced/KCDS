// The server browser's pings: how long a TCP connect to each listed server takes (its game port
// serves the server's status and resource files on TCP), measured off the browser's thread, at
// most kParallel at once and each given up after kTimeoutMs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kcds::runtime::session::pings {

// Starts measuring `addresses` (host:port) for the list `generation`; a list already measured
// or being measured is not started again.
void start(std::uint64_t generation, std::vector<std::string> addresses);

// The pings of the last list once all are measured (address -> ms; an address that did not
// answer is left out), once.
std::optional<std::map<std::string, int>> take_results();

}  // namespace kcds::runtime::session::pings
