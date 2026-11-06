// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "ai_bridge.hpp"

#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../core/log.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::ai_bridge {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr std::uint16_t kDefaultPort = 47870;
constexpr std::size_t kMostLine = 1u << 20;
constexpr std::size_t kMostWaiting = 64;
// The longest answer the editor's parts may make (a screenshot is a few hundred KiB).
constexpr std::size_t kMostAnswer = 32u << 20;

std::once_flag started;
std::atomic<bool> listening{false};
std::mutex bridge_mutex;
// The connection now (INVALID_SOCKET when none) and the request lines read from it.
SOCKET client = INVALID_SOCKET;
std::vector<std::string> requests;
std::string token;
// Answers to the editor's localArchitectAi questions, sent at the next pump.
std::vector<std::pair<std::int64_t, sdk::Value>> replies;
// The editor's answer so far: a message holds 64 KiB, so a longer one comes in parts, in order.
std::string answer_so_far;

std::int64_t whole_number(const sdk::Value& message, const char* name, std::int64_t fallback) {
  const sdk::Value* field = integration::script_field(message, name);
  if (field && field->tag() == sdk::Tag::Int) {
    return field->as_int();
  }
  if (field && field->tag() == sdk::Tag::Double) {
    return std::int64_t(field->as_double());
  }
  return fallback;
}

std::uint16_t port() {
  wchar_t value[8] = {};
  const DWORD length = GetEnvironmentVariableW(L"KCDS_ARCHITECT_AI_PORT", value, 8);
  const long wanted = length > 0 && length < 8 ? std::wcstol(value, nullptr, 10) : 0;
  return wanted > 1024 && wanted < 65536 ? std::uint16_t(wanted) : kDefaultPort;
}

// A new token, written where only this user reads it; empty when it could not be written.
std::string make_token() {
  std::random_device random;
  std::string made;
  constexpr char kHex[] = "0123456789abcdef";
  for (int index = 0; index < 32; ++index) {
    made.push_back(kHex[random() & 15u]);
  }
  wchar_t local[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0) {
    return std::string();
  }
  const std::filesystem::path folder = std::filesystem::path(local) / L"KCDS" / L"architect";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  std::ofstream file(folder / L"ai-token", std::ios::binary | std::ios::trunc);
  file << made;
  return file.good() ? made : std::string();
}

void write_line(SOCKET to, const std::string& text) {
  std::string line = text;
  line.push_back('\n');
  std::size_t sent = 0;
  while (sent < line.size()) {
    const int part = ::send(to, line.data() + sent, int(line.size() - sent), 0);
    if (part <= 0) {
      return;
    }
    sent += std::size_t(part);
  }
}

// Reads one connection until it closes: the token first, then request lines.
void serve(SOCKET connection) {
  std::string pending;
  bool trusted = false;
  char buffer[4096];
  while (true) {
    const int got = ::recv(connection, buffer, int(sizeof(buffer)), 0);
    if (got <= 0) {
      break;
    }
    pending.append(buffer, std::size_t(got));
    if (pending.size() > kMostLine) {
      break;
    }
    std::size_t end = 0;
    while ((end = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, end);
      pending.erase(0, end + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!trusted) {
        trusted = !token.empty() && line == token;
        if (!trusted) {
          kcds::game_client::log("ARCHITECT AI refused a connection (wrong token)");
          closesocket(connection);
          return;
        }
        {
          std::lock_guard<std::mutex> lock(bridge_mutex);
          client = connection;
        }
        write_line(connection, "ok");
        kcds::game_client::log("ARCHITECT AI connected");
        continue;
      }
      std::lock_guard<std::mutex> lock(bridge_mutex);
      if (requests.size() < kMostWaiting) {
        requests.push_back(std::move(line));
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(bridge_mutex);
    if (client == connection) {
      client = INVALID_SOCKET;
    }
  }
  closesocket(connection);
  kcds::game_client::log("ARCHITECT AI disconnected");
}

void listen_loop(SOCKET listener) {
  while (true) {
    const SOCKET connection = accept(listener, nullptr, nullptr);
    if (connection == INVALID_SOCKET) {
      return;
    }
    // One assistant at a time: the next connection waits until this one closes.
    serve(connection);
  }
}

void start() {
  token = make_token();
  WSADATA data{};
  if (token.empty() || WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    kcds::game_client::log("ARCHITECT AI off: no token file");
    return;
  }
  const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port());
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool bound = listener != INVALID_SOCKET &&
                     bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                     ::listen(listener, 1) == 0;
  if (!bound) {
    kcds::game_client::log("ARCHITECT AI off: port %u not free", unsigned(port()));
    if (listener != INVALID_SOCKET) {
      closesocket(listener);
    }
    return;
  }
  kcds::game_client::log("ARCHITECT AI listening on 127.0.0.1:%u", unsigned(port()));
  listening.store(true);
  std::thread(listen_loop, listener).detach();
}

// The MCP server shipped beside the game client: <its folder>/architect-mcp/server.cjs.
std::string server_path() {
  HMODULE module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&server_path), &module);
  wchar_t file[MAX_PATH] = {};
  if (!module || GetModuleFileNameW(module, file, MAX_PATH) == 0) {
    return std::string();
  }
  const std::filesystem::path server =
      std::filesystem::path(file).parent_path() / L"architect-mcp" / L"server.cjs";
  const std::u8string text = server.u8string();
  return std::string(text.begin(), text.end());
}

// The text on the Windows clipboard (the editor's Copy buttons: a Scaleform panel has no text
// selection).
void copy_text(const std::string& text) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  if (length <= 0 || !OpenClipboard(nullptr)) {
    return;
  }
  EmptyClipboard();
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (std::size_t(length) + 1) * sizeof(wchar_t));
  if (memory) {
    auto* wide = static_cast<wchar_t*>(GlobalLock(memory));
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide, length);
    wide[length] = L'\0';
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
      GlobalFree(memory);
    }
  }
  CloseClipboard();
}

// What the editor's AI tab shows: { port, listening, connected, server, token }.
sdk::Value info() {
  std::lock_guard<std::mutex> lock(bridge_mutex);
  return sdk::Value(sdk::Value::Map{
      {"port", sdk::Value(std::int64_t(port()))},
      {"listening", sdk::Value(listening.load())},
      {"connected", sdk::Value(client != INVALID_SOCKET)},
      {"server", sdk::Value(server_path())},
      {"token", sdk::Value(token)},
  });
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "syncRequest") {
    const sdk::Value* id = integration::script_field(message, "id");
    const sdk::Value* request = integration::script_field(message, "request");
    if (!id || id->tag() != sdk::Tag::Int || !request ||
        integration::script_operation(*request) != "localArchitectAi") {
      return false;
    }
    sdk::Value answer = local_mode::enabled() ? info() : sdk::Value();
    std::lock_guard<std::mutex> lock(bridge_mutex);
    replies.emplace_back(id->as_int(), std::move(answer));
    return true;
  }
  if (operation == "architectCopy") {
    const sdk::Value* text = integration::script_field(message, "text");
    if (local_mode::enabled() && text && text->tag() == sdk::Tag::Str) {
      copy_text(text->as_str());
    }
    return true;
  }
  if (operation != "architectReply") {
    return false;
  }
  const sdk::Value* text = integration::script_field(message, "text");
  if (!text || text->tag() != sdk::Tag::Str) {
    return true;
  }
  const std::int64_t part = whole_number(message, "part", 0);
  const std::int64_t parts = whole_number(message, "parts", 1);
  std::lock_guard<std::mutex> lock(bridge_mutex);
  if (part == 0) {
    answer_so_far.clear();
  }
  if (answer_so_far.size() + text->as_str().size() > kMostAnswer) {
    answer_so_far.clear();
    return true;
  }
  answer_so_far += text->as_str();
  if (part + 1 < parts) {
    return true;
  }
  if (client != INVALID_SOCKET) {
    write_line(client, answer_so_far);
  }
  answer_so_far.clear();
  return true;
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  if (!local_mode::enabled()) {
    return;
  }
  std::call_once(started, start);
  std::vector<std::string> taken;
  std::vector<std::pair<std::int64_t, sdk::Value>> answers;
  {
    std::lock_guard<std::mutex> lock(bridge_mutex);
    taken.swap(requests);
    answers.swap(replies);
  }
  for (auto& [id, value] : answers) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(id)},
                                    {"value", std::move(value)}}));
  }
  for (std::string& line : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("architectCommand"))},
                                    {"text", sdk::Value(std::move(line))}}));
  }
}

}  // namespace kcds::runtime::architect::ai_bridge
