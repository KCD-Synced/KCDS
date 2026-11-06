#define _CRT_RAND_S
#include "discord_login.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>

#include "kcd2s/platform/http_win.hpp"
#include "../discord/discord_presence.hpp"

namespace kcds::runtime::identity {
namespace {

namespace http = kcd2s::platform::http_win;

constexpr int kHttpTimeoutMs = 10000;
// A link ticket lives ten minutes on the Master; the game keeps it a little less.
constexpr double kTicketSeconds = 9 * 60;
constexpr int kMaxRequestBytes = 8192;
// How long the player has to answer the Discord app's window before the browser opens.
constexpr unsigned kDiscordAppAnswerMs = 120000;

std::mutex login_mutex;
std::string ticket_master;
std::string ticket_text;
double ticket_at = 0;
bool running = false;
std::optional<DiscordLoginResult> result;

double seconds_now() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void finish(DiscordLogin state, const std::string& message) {
  std::lock_guard<std::mutex> lock(login_mutex);
  running = false;
  result = DiscordLoginResult{state, message};
}

std::string escaped_json(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      out += c;
    }
  }
  return out + "\"";
}

// 32 random hex digits, so only the browser this game sent comes back with its answer.
std::string random_state() {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < 4; ++i) {
    unsigned int value = 0;
    rand_s(&value);
    for (int j = 0; j < 8; ++j) {
      out += kHex[(value >> (j * 4)) & 15];
    }
  }
  return out;
}

// The page the browser shows after coming back.
void answer_browser(SOCKET client, int status, const std::string& text) {
  const std::string body =
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>KCDS</title></head>"
      "<body style=\"font-family:sans-serif;background:#1b1712;color:#e8dcc4;text-align:center;"
      "padding-top:80px\"><h2>KCDS</h2><p>" +
      text + "</p></body></html>";
  const std::string head = "HTTP/1.1 " + std::to_string(status) +
                           (status == 200 ? " OK" : " Not Found") +
                           "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
  const std::string all = head + body;
  send(client, all.data(), int(all.size()), 0);
}

// One request of the browser, read until its head ends.
std::string read_request(SOCKET client) {
  std::string request;
  char buffer[1024];
  while (request.size() < kMaxRequestBytes && request.find("\r\n\r\n") == std::string::npos) {
    const int got = recv(client, buffer, sizeof(buffer), 0);
    if (got <= 0) {
      break;
    }
    request.append(buffer, std::size_t(got));
  }
  return request;
}

SOCKET open_listener() {
  const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    return INVALID_SOCKET;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(kDiscordLoginPort);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  // Only this game listens there: no other program may share the port.
  BOOL exclusive = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
             sizeof(exclusive));
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(listener, 4) != 0) {
    closesocket(listener);
    return INVALID_SOCKET;
  }
  return listener;
}

// Waits for the browser to come back with this state: the code, or "" with why not.
std::string wait_for_code(SOCKET listener, const std::string& state, std::string& problem,
                          Logger log) {
  const double deadline = seconds_now() + kDiscordLoginSeconds;
  while (seconds_now() < deadline) {
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(listener, &ready);
    timeval wait{1, 0};
    if (select(0, &ready, nullptr, nullptr, &wait) <= 0) {
      continue;
    }
    const SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      continue;
    }
    DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    DiscordCallback callback;
    if (!parse_discord_callback(read_request(client), callback)) {
      answer_browser(client, 404, "Nothing here.");
      closesocket(client);
      continue;
    }
    if (callback.state != state) {
      answer_browser(client, 200, "This sign-in is not the one KCDS started. Start it again.");
      closesocket(client);
      continue;
    }
    if (!callback.error.empty() || callback.code.empty()) {
      answer_browser(client, 200,
                     "Discord did not let KCDS know who you are. You can close this "
                     "page and go back to the game.");
      closesocket(client);
      log("DISCORD sign-in refused in the browser: %s", callback.error.c_str());
      problem = "You did not allow KCDS in Discord.";
      return {};
    }
    answer_browser(client, 200,
                   "Your Discord account is being verified. You can close this page and go back "
                   "to the game.");
    closesocket(client);
    return callback.code;
  }
  problem = "The Discord sign-in was not finished in time.";
  return {};
}

std::string text_field(const std::string& body, const char* name) {
  const std::string key = std::string("\"") + name + "\":\"";
  const std::size_t at = body.find(key);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t start = at + key.size();
  const std::size_t end = body.find('"', start);
  return end == std::string::npos ? std::string() : body.substr(start, end - start);
}

// The Discord app's main window: a visible top-level window of a Discord.exe process.
struct DiscordWindowSearch {
  HWND found = nullptr;
};

BOOL CALLBACK find_discord_window(HWND window, LPARAM argument) {
  auto* search = reinterpret_cast<DiscordWindowSearch*>(argument);
  if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr ||
      GetWindowTextLengthW(window) == 0) {
    return TRUE;
  }
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (!process) {
    return TRUE;
  }
  wchar_t path[MAX_PATH] = {};
  DWORD size = MAX_PATH;
  const bool named = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
  CloseHandle(process);
  const wchar_t* file = wcsrchr(path, L'\\');
  file = file ? file + 1 : path;
  if (named && _wcsicmp(file, L"Discord.exe") == 0) {
    search->found = window;
    return FALSE;
  }
  return TRUE;
}

// Brings the Discord app to the front, where its window asks the player to allow KCDS: the game
// may hand the foreground on while it has it.
void show_discord_app(Logger log) {
  DiscordWindowSearch search;
  EnumWindows(find_discord_window, reinterpret_cast<LPARAM>(&search));
  if (!search.found) {
    log("DISCORD app window not found to bring forward");
    return;
  }
  DWORD process_id = 0;
  GetWindowThreadProcessId(search.found, &process_id);
  AllowSetForegroundWindow(process_id);
  if (IsIconic(search.found)) {
    ShowWindow(search.found, SW_RESTORE);
  }
  SetForegroundWindow(search.found);
}

// The browser's sign-in: Discord's page opened, its answer taken on the listener. The code, or
// "" with why not in `problem`.
std::string browser_code(std::string& problem, Logger log) {
  WSADATA data;
  WSAStartup(MAKEWORD(2, 2), &data);
  const SOCKET listener = open_listener();
  if (listener == INVALID_SOCKET) {
    log("DISCORD sign-in: port %u is taken", unsigned(kDiscordLoginPort));
    problem =
        "Another program uses the port KCDS needs for the Discord sign-in (47811). Close it "
        "and try again.";
    WSACleanup();
    return {};
  }
  const std::string state = random_state();
  const std::string url = discord_login_url(discord::kDefaultAppId, state);
  const std::wstring wide(url.begin(), url.end());
  // The browser may come to the front over the game, so the player sees the page.
  AllowSetForegroundWindow(ASFW_ANY);
  ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  log("DISCORD sign-in page opened in the browser");
  const std::string code = wait_for_code(listener, state, problem, log);
  closesocket(listener);
  WSACleanup();
  return code;
}

DWORD WINAPI login(void* argument) {
  const auto log = reinterpret_cast<Logger>(argument);
  std::string master;
  std::string ticket;
  {
    std::lock_guard<std::mutex> lock(login_mutex);
    if (!ticket_text.empty() && seconds_now() - ticket_at < kTicketSeconds) {
      master = ticket_master;
      ticket = ticket_text;
    }
  }
  if (ticket.empty()) {
    log("DISCORD sign-in: no link ticket from the last licence proof");
    finish(DiscordLogin::Failed,
           "KCDS can't verify Discord accounts right now. Try joining again in a moment.");
    return 0;
  }
  // The Discord app's own window first, when it runs; else, or when the player closes it, the
  // browser.
  bool from_app = false;
  std::string problem;
  std::string code;
  if (!discord::user_id().empty()) {
    log("DISCORD sign-in asked in the Discord app");
    show_discord_app(log);
    code = discord::authorize(kDiscordAppAnswerMs);
    from_app = !code.empty();
  }
  if (code.empty()) {
    code = browser_code(problem, log);
  }
  if (code.empty()) {
    finish(DiscordLogin::Failed, problem);
    return 0;
  }
  // A code of the app's window was asked without a redirect; the Master trades it so.
  const std::string body = "{\"ticket\":" + escaped_json(ticket) +
                           ",\"code\":" + escaped_json(code) + (from_app ? ",\"app\":true}" : "}");
  const auto answer =
      http::request("POST", master + "/v1/discord/link", body, kHttpTimeoutMs, 4096);
  if (answer.status == 200) {
    {
      std::lock_guard<std::mutex> lock(login_mutex);
      ticket_text.clear();
    }
    log("DISCORD account verified");
    finish(DiscordLogin::Verified, {});
    return 0;
  }
  const std::string error = text_field(answer.body, "error");
  log("DISCORD verification refused by the Master (HTTP %d %s)", answer.status, error.c_str());
  finish(DiscordLogin::Failed, error == "ticketExpired"
                                   ? "The verification took too long. Join the server again, "
                                     "then verify."
                                   : "KCDS could not verify your Discord account. Try again.");
  return 0;
}

}  // namespace

void remember_discord_ticket(const std::string& master_url, const std::string& ticket) {
  std::lock_guard<std::mutex> lock(login_mutex);
  ticket_master = master_url;
  ticket_text = ticket;
  ticket_at = seconds_now();
}

bool start_discord_login(Logger log) {
  {
    std::lock_guard<std::mutex> lock(login_mutex);
    if (running) {
      return false;
    }
    running = true;
    result.reset();
  }
  const HANDLE thread = CreateThread(nullptr, 0, login, reinterpret_cast<void*>(log), 0, nullptr);
  if (!thread) {
    finish(DiscordLogin::Failed, "The game could not start the Discord sign-in.");
    return true;
  }
  CloseHandle(thread);
  return true;
}

std::optional<DiscordLoginResult> take_discord_login_result() {
  std::lock_guard<std::mutex> lock(login_mutex);
  std::optional<DiscordLoginResult> taken = result;
  result.reset();
  return taken;
}

}  // namespace kcds::runtime::identity
