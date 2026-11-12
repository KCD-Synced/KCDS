#pragma once
// The server's /metrics during a run (the resource host's Prometheus text, web/prometheus.cjs):
// fetched over plain HTTP every few seconds and read into the numbers a bot run reports (tick
// time, missed ticks, CPU and memory of the server and its resource host, egress, players and
// bots, refused checks). /metrics is served on the resources' HTTP port (KCDS_HTTP_PORT), and the
// server writes its numbers only with KCDS_SERVER_METRICS set (an interval in seconds); without it
// the tick numbers stay 0.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#ifdef _WIN32
#include "../../udp_win.hpp"
#else
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kcd2s::local::bots {

struct HttpUrl {
  std::string host;
  unsigned short port = 80;
  std::string path = "/";
};

// "http://host:port/path"; nullopt for anything else (https is not read).
inline std::optional<HttpUrl> parse_http_url(const std::string& text) {
  const std::string scheme = "http://";
  if (text.rfind(scheme, 0) != 0) {
    return std::nullopt;
  }
  HttpUrl url;
  const std::string rest = text.substr(scheme.size());
  const std::size_t slash = rest.find('/');
  const std::string authority = rest.substr(0, slash);
  url.path = slash == std::string::npos ? "/" : rest.substr(slash);
  const std::size_t colon = authority.rfind(':');
  url.host = authority.substr(0, colon);
  if (colon != std::string::npos) {
    const std::string port = authority.substr(colon + 1);
    char* end = nullptr;
    const long value = std::strtol(port.c_str(), &end, 10);
    if (port.empty() || *end != '\0' || value < 1 || value > 65535) {
      return std::nullopt;
    }
    url.port = static_cast<unsigned short>(value);
  }
  if (url.host.empty()) {
    return std::nullopt;
  }
  return url;
}

// The metrics by name with their labels ("kcds_tick_work_ms{stat=\"p99\"}"), from Prometheus text.
inline std::map<std::string, double> parse_prometheus(const std::string& text) {
  std::map<std::string, double> values;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const std::size_t space = line.rfind(' ');
    if (space == std::string::npos) {
      continue;
    }
    const std::string number = line.substr(space + 1);
    char* end = nullptr;
    const double value = std::strtod(number.c_str(), &end);
    if (end == number.c_str()) {
      continue;
    }
    values[line.substr(0, space)] = value;
  }
  return values;
}

// One reading of the server.
struct ServerSample {
  double elapsed_s = 0.0;  // since the run started
  bool ok = false;
  double tick_p50_ms = 0.0;
  double tick_p99_ms = 0.0;
  double tick_max_ms = 0.0;
  double missed_ticks = 0.0;        // in the server's last interval
  double missed_ticks_total = 0.0;  // since the server started
  double bytes_out_total = 0.0;     // since the server started
  double interval_s = 0.0;
  double cpu_percent = 0.0;  // of one core
  double rss_bytes = 0.0;
  double host_cpu_percent = 0.0;  // the resource host (Node.js)
  double host_rss_bytes = 0.0;
  double egress_bytes_per_s = 0.0;
  double players = 0.0;
  double bots = 0.0;
  double movement_refusals_total = 0.0;
  double melee_refusals_total = 0.0;
};

inline ServerSample read_server_sample(const std::map<std::string, double>& metrics) {
  auto value = [&](const std::string& name) {
    const auto found = metrics.find(name);
    return found == metrics.end() ? 0.0 : found->second;
  };
  ServerSample sample;
  sample.ok = metrics.count("kcds_players") != 0;
  sample.tick_p50_ms = value("kcds_tick_work_ms{stat=\"p50\"}");
  sample.tick_p99_ms = value("kcds_tick_work_ms{stat=\"p99\"}");
  sample.tick_max_ms = value("kcds_tick_work_ms{stat=\"max\"}");
  sample.missed_ticks = value("kcds_tick_missed_deadlines");
  sample.missed_ticks_total = value("kcds_since_start_missed_deadlines");
  sample.bytes_out_total = value("kcds_udp_total_bytes_out");
  sample.interval_s = value("kcds_interval_s");
  sample.cpu_percent = value("kcds_process_cpu_percent_of_one_core");
  sample.rss_bytes = value("kcds_process_working_set_bytes");
  sample.host_cpu_percent = value("kcds_host_cpu_percent_of_one_core");
  sample.host_rss_bytes = value("kcds_host_rss_bytes");
  if (sample.interval_s > 0.0) {
    sample.egress_bytes_per_s = value("kcds_udp_interval_bytes_out") / sample.interval_s;
  }
  sample.players = value("kcds_players");
  sample.bots = value("kcds_players_by_kind{kind=\"bot\"}");
  sample.movement_refusals_total = value("kcds_checks_movement_refusals_total");
  sample.melee_refusals_total = value("kcds_checks_melee_refusals_total");
  return sample;
}

// GET over HTTP/1.0; the body, or nullopt when the server could not be read within `timeout`.
inline std::optional<std::string> http_get(const HttpUrl& url, double timeout_s = 2.0) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &found) != 0 ||
      !found) {
    return std::nullopt;
  }
#ifdef _WIN32
  SOCKET handle = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
  const bool valid = handle != INVALID_SOCKET;
  const DWORD wait = DWORD(timeout_s * 1000.0);
  if (valid) {
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wait), sizeof(wait));
    setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&wait), sizeof(wait));
  }
#else
  int handle = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
  const bool valid = handle >= 0;
  timeval wait{};
  wait.tv_sec = long(timeout_s);
  wait.tv_usec = long((timeout_s - double(wait.tv_sec)) * 1e6);
  if (valid) {
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof(wait));
  }
#endif
  auto close_handle = [&] {
#ifdef _WIN32
    closesocket(handle);
#else
    close(handle);
#endif
  };
  if (!valid) {
    freeaddrinfo(found);
    return std::nullopt;
  }
  const bool connected = connect(handle, found->ai_addr, int(found->ai_addrlen)) == 0;
  freeaddrinfo(found);
  if (!connected) {
    close_handle();
    return std::nullopt;
  }
  const std::string request = "GET " + url.path + " HTTP/1.0\r\nHost: " + url.host +
                              "\r\nUser-Agent: kcds-bots\r\nConnection: close\r\n\r\n";
  if (send(handle, request.data(), int(request.size()), 0) != int(request.size())) {
    close_handle();
    return std::nullopt;
  }
  std::string response;
  char buffer[8192];
  for (;;) {
    const auto got = recv(handle, buffer, int(sizeof(buffer)), 0);
    if (got <= 0) {
      break;
    }
    response.append(buffer, std::size_t(got));
    if (response.size() > 8 * 1024 * 1024) {
      break;
    }
  }
  close_handle();
  const std::size_t body = response.find("\r\n\r\n");
  if (response.rfind("HTTP/1.", 0) != 0 || body == std::string::npos ||
      response.find(" 200 ") > response.find("\r\n")) {
    return std::nullopt;
  }
  return response.substr(body + 4);
}

}  // namespace kcd2s::local::bots
