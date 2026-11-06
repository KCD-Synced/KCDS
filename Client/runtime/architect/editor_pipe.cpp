// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "editor_pipe.hpp"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "../core/log.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::editor_pipe {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// A line longer than this is refused (a step is a short command).
constexpr std::size_t kMaxLine = 4096;

std::mutex queue_mutex;
std::vector<Step> inbound;
std::deque<std::string> outbound;
HANDLE wake = nullptr;
std::atomic<bool> started{false};

// The text of a JSON string value after "key": in a line (the MCP server writes plain JSON).
bool string_field(const std::string& json, const char* key, std::string& out) {
  const std::string marker = std::string("\"") + key + "\"";
  std::size_t at = json.find(marker);
  if (at == std::string::npos) {
    return false;
  }
  at = json.find(':', at + marker.size());
  if (at == std::string::npos) {
    return false;
  }
  at = json.find('"', at);
  if (at == std::string::npos) {
    return false;
  }
  out.clear();
  for (std::size_t index = at + 1; index < json.size(); ++index) {
    const char c = json[index];
    if (c == '"') {
      return true;
    }
    if (c == '\\' && index + 1 < json.size()) {
      const char next = json[++index];
      out.push_back(next == 'n' ? '\n' : next == 't' ? '\t' : next);
      continue;
    }
    out.push_back(c);
  }
  return false;
}

bool number_field(const std::string& json, const char* key, std::int64_t& out) {
  const std::string marker = std::string("\"") + key + "\"";
  std::size_t at = json.find(marker);
  if (at == std::string::npos) {
    return false;
  }
  at = json.find(':', at + marker.size());
  if (at == std::string::npos) {
    return false;
  }
  return std::sscanf(json.c_str() + at + 1, " %lld", &out) == 1;
}

// A JSON string of the text (named apart from std::quoted, which a std::string argument finds).
std::string json_text(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out + "\"";
}

void take_line(const std::string& line) {
  std::int64_t id = 0;
  std::string step;
  if (!number_field(line, "id", id) || !string_field(line, "line", step) || step.empty()) {
    const std::string refused =
        "{\"id\":0,\"text\":" + json_text("refused: a line is JSON with id and line") + "}\n";
    std::lock_guard<std::mutex> lock(queue_mutex);
    outbound.push_back(refused);
    SetEvent(wake);
    return;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  inbound.push_back(Step{id, step});
}

// One client at a time: read its lines and write the answers, until it goes.
void serve(HANDLE pipe) {
  OVERLAPPED reading{};
  reading.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::string pending;
  char buffer[1024];
  bool alive = true;
  while (alive) {
    DWORD got = 0;
    ResetEvent(reading.hEvent);
    const BOOL done = ReadFile(pipe, buffer, sizeof(buffer), &got, &reading);
    if (!done && GetLastError() != ERROR_IO_PENDING) {
      break;
    }
    HANDLE waits[2] = {reading.hEvent, wake};
    for (;;) {
      const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (which == WAIT_OBJECT_0 + 1) {
        // Answers to write.
        std::deque<std::string> lines;
        {
          std::lock_guard<std::mutex> lock(queue_mutex);
          lines.swap(outbound);
        }
        for (const std::string& line : lines) {
          DWORD written = 0;
          OVERLAPPED writing{};
          writing.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
          if (!WriteFile(pipe, line.data(), DWORD(line.size()), nullptr, &writing) &&
              GetLastError() != ERROR_IO_PENDING) {
            alive = false;
          } else {
            GetOverlappedResult(pipe, &writing, &written, TRUE);
          }
          CloseHandle(writing.hEvent);
        }
        if (!alive) {
          break;
        }
        continue;
      }
      if (!GetOverlappedResult(pipe, &reading, &got, FALSE) || got == 0) {
        alive = false;
        break;
      }
      pending.append(buffer, got);
      std::size_t end;
      while ((end = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, end);
        pending.erase(0, end + 1);
        if (!line.empty() && line.size() <= kMaxLine) {
          take_line(line);
        }
      }
      if (pending.size() > kMaxLine) {
        pending.clear();
      }
      break;
    }
  }
  CancelIo(pipe);
  CloseHandle(reading.hEvent);
}

void run() {
  wchar_t name[96];
  swprintf_s(name, L"\\\\.\\pipe\\kcds-editor-%lu", GetCurrentProcessId());
  for (;;) {
    const DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    const DWORD pipe_mode =
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    HANDLE pipe = CreateNamedPipeW(name, open_mode, pipe_mode, 1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      game_client::log("EDITOR AI bridge: no pipe (%lu)", GetLastError());
      return;
    }
    const BOOL connected =
        ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (connected) {
      game_client::log("EDITOR AI bridge: an assistant connected");
      serve(pipe);
      game_client::log("EDITOR AI bridge: the assistant left");
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

}  // namespace

void start() {
  if (!local_mode::enabled() || started.exchange(true)) {
    return;
  }
  wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::thread(run).detach();
  game_client::log("EDITOR AI bridge open: \\\\.\\pipe\\kcds-editor-%lu", GetCurrentProcessId());
}

std::vector<Step> take() {
  std::lock_guard<std::mutex> lock(queue_mutex);
  std::vector<Step> steps;
  steps.swap(inbound);
  return steps;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "editorReply") {
    return false;
  }
  std::int64_t id = 0;
  if (const sdk::Value* field = integration::script_field(message, "id")) {
    if (field->tag() == sdk::Tag::Int) {
      id = field->as_int();
    } else if (field->tag() == sdk::Tag::Double) {
      id = std::int64_t(field->as_double());
    }
  }
  std::string text;
  if (const sdk::Value* field = integration::script_field(message, "text")) {
    if (field->tag() == sdk::Tag::Str) {
      text = field->as_str();
    }
  }
  if (!started.load()) {
    return true;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  outbound.push_back("{\"id\":" + std::to_string(id) + ",\"text\":" + json_text(text) + "}\n");
  SetEvent(wake);
  return true;
}

}  // namespace kcds::runtime::architect::editor_pipe
