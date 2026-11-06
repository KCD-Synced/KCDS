// MasterClient — talks to the master server's HTTP directory. The HTTP transport is injected
// (std::function) so the parse/encode logic is headless-testable with a fake; the real client build
// supplies an HTTPS implementation. Master server endpoints: GET /servers, POST /heartbeat,
// POST /deregister.
#ifndef KCD2S_PLATFORM_MASTER_CLIENT_HPP
#define KCD2S_PLATFORM_MASTER_CLIENT_HPP

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/platform/server_browser.hpp"

namespace kcd2s::platform {

class MasterClient {
 public:
  using HttpGet = std::function<std::optional<std::string>(const std::string& url)>;
  using HttpPost = std::function<bool(const std::string& url, const std::string& body)>;

  MasterClient(std::string base_url, HttpGet get, HttpPost post)
      : base_(std::move(base_url)), get_(std::move(get)), post_(std::move(post)) {
  }

  // GET {base}/servers and parse. nullopt on transport failure or malformed JSON.
  std::optional<std::vector<ServerEntry>> fetch_servers() const {
    if (!get_)
      return std::nullopt;
    auto body = get_(base_ + "/servers");
    if (!body)
      return std::nullopt;
    return parse_server_list(*body);
  }

  // POST {base}/heartbeat with this server's entry. Returns transport success.
  bool heartbeat(const ServerEntry& e) const {
    return post_ && post_(base_ + "/heartbeat", encode_heartbeat(e));
  }

  // POST {base}/deregister on graceful shutdown.
  bool deregister(const std::string& id) const {
    std::string body = "{\"id\":\"";
    append_json_string(body, id);
    body += "\"}";
    return post_ && post_(base_ + "/deregister", body);
  }

 private:
  std::string base_;
  HttpGet get_;
  HttpPost post_;
};

}  // namespace kcd2s::platform

#endif  // KCD2S_PLATFORM_MASTER_CLIENT_HPP
