#include "discord_link.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <mutex>

#include "discord_ipc.hpp"

namespace kcds::loader::discord {
namespace {

constexpr DWORD kTickMs = 250;
constexpr double kRetrySeconds = 15.0;
constexpr double kMinUpdateSeconds = 4.0;
constexpr double kReassertSeconds = 30.0;

// An authorization: asked for by a client, sent and answered on the worker.
enum class Asked { none, wanted, sent, answered, failed };

std::mutex state_mutex;
bool started = false;
std::string app;
Log log_line = nullptr;
std::string wanted_activity;
bool connected_now = false;
std::string signed_in_user;
Asked asked = Asked::none;
std::string answer_code;

void logf(const char* format, const std::string& text) {
  if (!log_line) {
    return;
  }
  char line[512];
  std::snprintf(line, sizeof(line), format, text.c_str());
  log_line(line);
}

double seconds_now() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void set_connected(bool connected, const std::string& user) {
  std::lock_guard<std::mutex> lock(state_mutex);
  connected_now = connected;
  signed_in_user = user;
  if (!connected && (asked == Asked::wanted || asked == Asked::sent)) {
    asked = Asked::failed;
  }
}

// One connection to the Discord app.
class Connection {
 public:
  ~Connection() {
    close();
  }

  bool open(const std::string& id) {
    for (int index = 0; index < 10 && pipe_ == INVALID_HANDLE_VALUE; ++index) {
      const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(index);
      pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                          nullptr);
    }
    if (pipe_ == INVALID_HANDLE_VALUE) {
      return false;
    }
    if (!write(kOpHandshake, handshake_json(id))) {
      close();
      return false;
    }
    // Discord answers at once: READY, or a close with its reason. The answer is read whole even
    // when Discord closes the pipe right after it.
    for (int frames = 0; frames < 4; ++frames) {
      std::uint32_t op = 0;
      std::string body;
      if (!read_frame(op, body)) {
        if (log_line) {
          log_line("DISCORD handshake got no answer");
        }
        break;
      }
      if (op == kOpFrame) {
        user_id_ = ready_user_id(body);
        if (!user_id_.empty()) {
          return true;
        }
      } else if (op == kOpClose) {
        // The worker asks again every 15 seconds: the same reason is logged once.
        const std::string reason = body.substr(0, 200);
        if (reason != last_refusal_) {
          logf("DISCORD refused: %s", reason);
        }
        last_refusal_ = reason;
        break;
      }
    }
    close();
    return false;
  }

  bool connected() const {
    return pipe_ != INVALID_HANDLE_VALUE;
  }

  const std::string& user_id() const {
    return user_id_;
  }

  bool write(std::uint32_t op, const std::string& body) {
    const std::vector<std::uint8_t> bytes = frame(op, body);
    DWORD written = 0;
    if (!WriteFile(pipe_, bytes.data(), DWORD(bytes.size()), &written, nullptr) ||
        written != bytes.size()) {
      close();
      return false;
    }
    return true;
  }

  // 1 with a frame, 0 when none waits, -1 when the pipe is gone or the frame is not Discord's.
  int read(std::uint32_t& op, std::string& body) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
      close();
      return -1;
    }
    if (available < 8) {
      return 0;
    }
    return read_frame(op, body) ? 1 : -1;
  }

  // The next frame, waiting for it.
  bool read_frame(std::uint32_t& op, std::string& body) {
    std::uint8_t head[8];
    if (!read_exactly(head, sizeof(head))) {
      return false;
    }
    op = std::uint32_t(head[0]) | std::uint32_t(head[1]) << 8 | std::uint32_t(head[2]) << 16 |
         std::uint32_t(head[3]) << 24;
    const std::uint32_t length = std::uint32_t(head[4]) | std::uint32_t(head[5]) << 8 |
                                 std::uint32_t(head[6]) << 16 | std::uint32_t(head[7]) << 24;
    if (length > kMaxFrameBytes) {
      close();
      return false;
    }
    body.assign(length, '\0');
    return length == 0 || read_exactly(body.data(), length);
  }

  void close() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
    }
  }

 private:
  bool read_exactly(void* into, DWORD size) {
    auto* bytes = static_cast<std::uint8_t*>(into);
    DWORD done = 0;
    while (done < size) {
      DWORD got = 0;
      if (!ReadFile(pipe_, bytes + done, size - done, &got, nullptr) || got == 0) {
        close();
        return false;
      }
      done += got;
    }
    return true;
  }

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::string user_id_;
  std::string last_refusal_;
};

// Whether an authorization waits to be sent; marks it sent.
bool take_authorization() {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (asked != Asked::wanted) {
    return false;
  }
  asked = Asked::sent;
  return true;
}

void finish_authorization(const std::string& code) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (asked != Asked::sent) {
    return;
  }
  asked = code.empty() ? Asked::failed : Asked::answered;
  answer_code = code;
}

// Discord's answers to what was sent: pings, a close, an authorization, an accepted or refused
// activity.
void read_answers(Connection& connection, bool& accepted_logged) {
  std::uint32_t op = 0;
  std::string body;
  int got = connection.read(op, body);
  while (got > 0) {
    if (op == kOpPing) {
      connection.write(kOpPong, body);
    } else if (op == kOpClose) {
      connection.close();
    } else if (is_authorize_answer(body)) {
      const std::string code = authorize_code(body);
      if (code.empty()) {
        logf("DISCORD authorization declined: %.200s", body);
      } else if (log_line) {
        log_line("DISCORD authorization granted");
      }
      finish_authorization(code);
    } else if (body.find("\"evt\":\"ERROR\"") != std::string::npos) {
      // Discord refused an activity (a field it does not take): its answer says which.
      logf("DISCORD refused: %.300s", body);
    } else if (body.find("\"cmd\":\"SET_ACTIVITY\"") != std::string::npos && !accepted_logged) {
      accepted_logged = true;
      // Discord's answer echoes the activity it took; its buttons are only their labels (Discord
      // shows them to the other users, never on the player's own profile).
      const std::size_t at = body.find("\"buttons\"");
      const std::size_t end = at == std::string::npos ? at : body.find(']', at);
      logf("DISCORD accepted, buttons %s",
           end == std::string::npos ? std::string("none") : body.substr(at, end - at + 1));
    }
    got = connection.read(op, body);
  }
}

DWORD WINAPI worker(void*) {
  std::string id;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    id = app;
  }
  Connection connection;
  double next_attempt = 0;
  double last_update = -kMinUpdateSeconds;
  std::string published;
  bool accepted_logged = false;
  std::uint64_t nonce = 1;
  const auto pid = std::uint32_t(GetCurrentProcessId());
  // The loader stays for the whole game: so does its connection.
  for (;;) {
    const double now = seconds_now();
    if (!connection.connected()) {
      if (now >= next_attempt) {
        next_attempt = now + kRetrySeconds;
        if (connection.open(id)) {
          published.clear();
          set_connected(true, connection.user_id());
          if (log_line) {
            log_line("DISCORD connected");
          }
        }
      }
      if (!connection.connected()) {
        set_connected(false, {});
        Sleep(kTickMs);
        continue;
      }
    }
    read_answers(connection, accepted_logged);
    std::string wanted;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      wanted = wanted_activity;
    }
    if (connection.connected() &&
        publish_due(wanted, published, now, last_update, kMinUpdateSeconds, kReassertSeconds)) {
      if (connection.write(kOpFrame, set_activity_json(pid, wanted, nonce++))) {
        if (wanted != published) {
          logf("DISCORD activity %s", wanted);
          accepted_logged = false;
        }
        published = wanted;
        last_update = now;
      }
    }
    // Discord shows the player its window to let KCDS know who they are.
    if (connection.connected() && take_authorization()) {
      if (!connection.write(kOpFrame, authorize_json(id, nonce++))) {
        finish_authorization({});
      } else if (log_line) {
        log_line("DISCORD authorization asked");
      }
    }
    if (!connection.connected()) {
      set_connected(false, {});
      if (log_line) {
        log_line("DISCORD disconnected");
      }
    }
    Sleep(kTickMs);
  }
}

}  // namespace

void start(const std::string& app_id, Log log) {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (started) {
      return;
    }
    started = true;
    app = app_id;
    log_line = log;
  }
  const HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
  if (thread) {
    CloseHandle(thread);
  }
}

void set_activity(const std::string& activity_json) {
  std::lock_guard<std::mutex> lock(state_mutex);
  wanted_activity = activity_json;
}

bool status(std::string& user_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  user_id = connected_now ? signed_in_user : std::string();
  return connected_now;
}

bool authorize(bool ask) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!ask) {
    asked = Asked::none;
    answer_code.clear();
    return true;
  }
  if (!connected_now) {
    return false;
  }
  // One request at a time: a second asker waits for the same answer.
  if (asked != Asked::wanted && asked != Asked::sent) {
    asked = Asked::wanted;
    answer_code.clear();
  }
  return true;
}

int authorization(std::string& code) {
  std::lock_guard<std::mutex> lock(state_mutex);
  // The answer stays for every client thread waiting on it, until the next authorization.
  if (asked == Asked::answered) {
    code = answer_code;
    return 1;
  }
  return asked == Asked::failed ? -1 : 0;
}

}  // namespace kcds::loader::discord
