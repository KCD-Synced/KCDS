// Client-side server browser — the model behind the custom start
// menu's server list. Parses the master server's /servers JSON (the Go directory's Entry shape),
// encodes a /heartbeat body, and offers filter + sort over the live list. Pure + headless-testable;
// the actual HTTP lives in MasterClient (injectable) so this has no network dependency.
#ifndef KCD2S_PLATFORM_SERVER_BROWSER_HPP
#define KCD2S_PLATFORM_SERVER_BROWSER_HPP

#include <optional>
#include <string>
#include <vector>

namespace kcd2s::platform {

// A master server list entry (public directory metadata only).
struct ServerEntry {
  std::string id;
  std::string name;
  std::string address;  // host:port for direct connect
  std::string gamemode;
  std::string map;  // level the server runs
  int players = 0;
  int maxPlayers = 0;
  std::string version;  // KCDS server release
  std::vector<std::string> tags;
  int protocol = 0;       // KSTP protocol version (client-compat gate)
  std::string image;      // optional https URL of the card art (validated by the master)
  bool queue = false;     // the server queues players when it is full
  bool password = false;  // joining asks for a password
  std::string lastSeen;   // ISO timestamp (opaque to the client)
};

// Parse a /servers JSON array. Returns nullopt on malformed input (treat as untrusted).
std::optional<std::vector<ServerEntry>> parse_server_list(const std::string& json);

// Encode a /heartbeat body for this server (JSON matching the Go Heartbeat tags).
std::string encode_heartbeat(const ServerEntry& e);

// Encode a server list as the JSON array the in-game menu page renders (the /servers shape).
std::string encode_server_list(const std::vector<ServerEntry>& servers);

// Append s to out as the contents of a JSON string (quotes not included).
void append_json_string(std::string& out, const std::string& s);

// Filterable / sortable view over a server list for the menu.
class ServerBrowser {
 public:
  enum class Sort { Players, Name };

  void set_servers(std::vector<ServerEntry> servers) {
    servers_ = std::move(servers);
  }
  std::size_t total() const {
    return servers_.size();
  }
  // The full (unfiltered) list — e.g. to resolve a selection by id regardless of the active filter.
  const std::vector<ServerEntry>& all() const {
    return servers_;
  }

  void set_text_filter(std::string substr) {
    text_ = std::move(substr);
  }
  void set_required_tag(std::string tag) {
    tag_ = std::move(tag);
  }
  void set_hide_full(bool v) {
    hide_full_ = v;
  }
  void set_protocol(int own_protocol) {
    protocol_ = own_protocol;
  }  // 0 = no compat filter
  void set_sort(Sort s, bool descending) {
    sort_ = s;
    descending_ = descending;
  }

  // The filtered + sorted list to show.
  std::vector<ServerEntry> view() const;

 private:
  std::vector<ServerEntry> servers_;
  std::string text_;
  std::string tag_;
  bool hide_full_ = false;
  int protocol_ = 0;
  Sort sort_ = Sort::Players;
  bool descending_ = true;
};

}  // namespace kcd2s::platform

#endif  // KCD2S_PLATFORM_SERVER_BROWSER_HPP
