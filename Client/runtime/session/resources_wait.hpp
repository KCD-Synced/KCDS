// How long the server's world has waited for the client resources. The wait starts again for each
// world revision and for each join: a rejoin in the same game gets the same revision from a new
// client, and must wait for that client's resources too.
#pragma once

#include <cstdint>

namespace kcds::game_client {

class ResourcesWait {
 public:
  // Starts the wait when the world or the client is new; returns true when it started.
  bool begin(std::uint32_t revision, std::uint64_t client, double now) {
    if (started_ && revision == revision_ && client == client_) {
      return false;
    }
    started_ = true;
    revision_ = revision;
    client_ = client;
    since_ = now;
    reported_ = false;
    return true;
  }
  // How long the current wait has lasted.
  double waited(double now) const {
    return now - since_;
  }
  // Returns true once per wait, to log that the world loads without the resources.
  bool report_once() {
    if (reported_) {
      return false;
    }
    reported_ = true;
    return true;
  }

 private:
  bool started_ = false;
  bool reported_ = false;
  std::uint32_t revision_ = 0;
  std::uint64_t client_ = 0;
  double since_ = 0;
};

}  // namespace kcds::game_client
