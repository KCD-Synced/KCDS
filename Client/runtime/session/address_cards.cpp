#include "address_cards.hpp"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "kcds/launcher/server_address.hpp"
#include "server_pings.hpp"

namespace kcds::runtime::session::address_cards {
namespace {

using Card = kcd2s::platform::MenuSession::AddressCard;

constexpr int kParallel = 4;
constexpr int kTimeoutMs = 3000;
constexpr DWORD kMostCardBytes = 65536;

std::mutex state_mutex;
std::uint64_t started_generation = 0;
bool running = false;
std::optional<std::vector<Card>> finished;

struct Handle {
  HINTERNET value = nullptr;
  ~Handle() {
    if (value) {
      WinHttpCloseHandle(value);
    }
  }
};

// The text of GET http://host:port/kcds/info.json, or "" when the server did not answer it.
std::string fetch_card(const std::string& host, std::uint16_t port) {
  Handle session{WinHttpOpen(L"KCDS", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.value) {
    return {};
  }
  WinHttpSetTimeouts(session.value, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
  const std::wstring wide_host(host.begin(), host.end());
  Handle connection{WinHttpConnect(session.value, wide_host.c_str(), port, 0)};
  Handle request{connection.value
                     ? WinHttpOpenRequest(connection.value, L"GET", L"/kcds/info.json", nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                     : nullptr};
  if (!request.value ||
      !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                          0, 0, 0) ||
      !WinHttpReceiveResponse(request.value, nullptr)) {
    return {};
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
      status != 200) {
    return {};
  }
  std::string text;
  char piece[4096];
  for (;;) {
    DWORD read = 0;
    if (!WinHttpReadData(request.value, piece, sizeof(piece), &read)) {
      return {};
    }
    if (read == 0) {
      break;
    }
    if (text.size() + read > kMostCardBytes) {
      return {};
    }
    text.append(piece, read);
  }
  return text;
}

Card ask(const std::string& address) {
  Card card;
  card.address = address;
  std::string host;
  std::uint16_t port = 0;
  if (!kcds::launcher::server_address::parse(address, host, port)) {
    return card;
  }
  card.ping = pings::measure(address);
  if (card.ping < 0) {
    return card;  // nothing answers there
  }
  const std::string text = fetch_card(host, port);
  if (!text.empty()) {
    kcd2s::platform::MenuSession::read_address_card(text, card);
  }
  return card;
}

void ask_all(std::vector<std::string> addresses) {
  std::vector<Card> cards(addresses.size());
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> workers;
  for (int i = 0; i < kParallel; ++i) {
    workers.emplace_back([&] {
      for (std::size_t at = next++; at < addresses.size(); at = next++) {
        cards[at] = ask(addresses[at]);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  finished = std::move(cards);
  running = false;
}

}  // namespace

void start(std::uint64_t generation, std::vector<std::string> addresses) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (running || generation == started_generation || addresses.empty()) {
    return;
  }
  if (addresses.size() > kcd2s::platform::MenuSession::kMostAddressFavourites) {
    addresses.resize(kcd2s::platform::MenuSession::kMostAddressFavourites);
  }
  started_generation = generation;
  running = true;
  std::thread(ask_all, std::move(addresses)).detach();
}

std::optional<std::vector<Card>> take_results() {
  std::lock_guard<std::mutex> lock(state_mutex);
  std::optional<std::vector<Card>> results;
  results.swap(finished);
  return results;
}

}  // namespace kcds::runtime::session::address_cards
