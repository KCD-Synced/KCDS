#include "discord_presence.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::discord {
namespace {

namespace sdk = kcd2s::sdk;

constexpr std::uint32_t kOpHandshake = 0;
constexpr std::uint32_t kOpFrame = 1;
constexpr std::uint32_t kOpClose = 2;
constexpr std::uint32_t kOpPing = 3;
constexpr std::uint32_t kOpPong = 4;
// Discord's frames are small; anything larger is not Discord.
constexpr std::uint32_t kMaxFrameBytes = 64 * 1024;
constexpr DWORD kTickMs = 250;
constexpr double kRetrySeconds = 15.0;
constexpr double kMinUpdateSeconds = 4.0;
// How often an unchanged activity is set again (Discord takes 5 updates in 20 seconds).
constexpr double kReassertSeconds = 30.0;

struct Status {
  bool available = false;
  std::string user_id;

  friend bool operator==(const Status& a, const Status& b) {
    return a.available == b.available && a.user_id == b.user_id;
  }
};

std::mutex state_mutex;
Presence presence;
Status status;
std::string version_text;
Logger log_line = nullptr;
bool started = false;
// An authorization: asked for by any thread, sent and answered on the worker.
struct Authorization {
  bool wanted = false;
  bool sent = false;
  bool done = false;
  std::string code;
};
Authorization authorization;
std::condition_variable authorization_changed;
// The runtime that was last told the status, and what it was told.
const void* told_session = nullptr;
std::optional<Status> told_status;

double seconds_now() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::int64_t unix_now() {
  using clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::seconds>(clock::now().time_since_epoch()).count();
}

std::string app_id() {
  char text[64] = {};
  const DWORD length = GetEnvironmentVariableA("KCDS_DISCORD_APP_ID", text, sizeof(text));
  if (length > 0 && length < sizeof(text)) {
    return text;
  }
  return kDefaultAppId;
}

void set_status(bool available, const std::string& user_id) {
  std::lock_guard<std::mutex> lock(state_mutex);
  status.available = available;
  status.user_id = user_id;
}

// Ends the authorization being waited for with Discord's code, "" when there is none.
void finish_authorization(const std::string& code) {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!authorization.wanted) {
      return;
    }
    authorization.wanted = false;
    authorization.sent = false;
    authorization.done = true;
    authorization.code = code;
  }
  authorization_changed.notify_all();
}

// Whether an authorization waits to be sent; marks it sent.
bool take_authorization() {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!authorization.wanted || authorization.sent) {
    return false;
  }
  authorization.sent = true;
  return true;
}

std::string current_user_id() {
  std::lock_guard<std::mutex> lock(state_mutex);
  return status.available ? status.user_id : std::string();
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
        if (log_line && reason != last_refusal_) {
          log_line("DISCORD refused: %s", reason.c_str());
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

DWORD WINAPI worker(void*) {
  const std::string id = app_id();
  if (id.empty()) {
    if (log_line) {
      log_line("DISCORD no application id (KCDS_DISCORD_APP_ID): presence off");
    }
    return 0;
  }
  Connection connection;
  double next_attempt = 0;
  double last_update = -kMinUpdateSeconds;
  std::string published;
  // Discord's answer to a changed activity is logged once, not at every repeat.
  bool accepted_logged = false;
  std::uint64_t nonce = 1;
  const auto pid = std::uint32_t(GetCurrentProcessId());
  for (;;) {
    const double now = seconds_now();
    if (!connection.connected()) {
      if (now >= next_attempt) {
        next_attempt = now + kRetrySeconds;
        if (connection.open(id)) {
          published.clear();
          set_status(true, connection.user_id());
          if (log_line) {
            log_line("DISCORD connected");
          }
        }
      }
      if (!connection.connected()) {
        set_status(false, {});
        Sleep(kTickMs);
        continue;
      }
    }
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
        if (log_line) {
          log_line(code.empty() ? "DISCORD authorization declined: %.200s"
                                : "DISCORD authorization granted",
                   body.c_str());
        }
        finish_authorization(code);
      } else if (body.find("\"evt\":\"ERROR\"") != std::string::npos && log_line) {
        // Discord refused an activity (a field it does not take): its answer says which.
        log_line("DISCORD refused: %.300s", body.c_str());
      } else if (body.find("\"cmd\":\"SET_ACTIVITY\"") != std::string::npos && log_line &&
                 !accepted_logged) {
        accepted_logged = true;
        // Discord's answer echoes the activity it took; its buttons are only their labels (Discord
        // shows them to the other users, never on the player's own profile).
        const std::size_t at = body.find("\"buttons\"");
        const std::size_t end = at == std::string::npos ? at : body.find(']', at);
        log_line("DISCORD accepted, buttons %s",
                 end == std::string::npos ? "none" : body.substr(at, end - at + 1).c_str());
      }
      got = connection.read(op, body);
    }
    std::string wanted;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      wanted = activity_json(presence, version_text);
    }
    // An unchanged activity is set again now and then: Discord shows the activity set last, and a
    // clip recorder (Medal) that sets its own for the game took the player's profile until KCDS's
    // activity next changed.
    const bool stale = now - last_update >= kReassertSeconds;
    if (connection.connected() && (wanted != published || stale) &&
        now - last_update >= kMinUpdateSeconds) {
      if (connection.write(kOpFrame, set_activity_json(pid, wanted, nonce++))) {
        if (log_line && wanted != published) {
          log_line("DISCORD activity %s", wanted.c_str());
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
      set_status(false, {});
      finish_authorization({});
      if (log_line) {
        log_line("DISCORD disconnected");
      }
    }
    Sleep(kTickMs);
  }
}

std::string text(const sdk::Value& message, const char* name) {
  const sdk::Value* value = kcd2s::integration::script_field(message, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string{};
}

}  // namespace

void start(const std::string& version, Logger log) {
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (started) {
      return;
    }
    started = true;
    version_text = version;
    log_line = log;
    presence.since = unix_now();
  }
  const HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
  if (thread) {
    CloseHandle(thread);
  }
}

void set_scene(Scene scene, const std::string& server) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (presence.scene == scene && presence.server == server) {
    return;
  }
  const bool same_server = (scene == Scene::Playing || scene == Scene::Joining) &&
                           (presence.scene == Scene::Playing || presence.scene == Scene::Joining) &&
                           presence.server == server;
  if (!same_server) {
    presence.script = ScriptPresence{};
    presence.players = 0;
    presence.max_players = 0;
  }
  presence.scene = scene;
  presence.server = server;
  presence.since = unix_now();
}

void entered_world() {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (presence.scene == Scene::Playing) {
    return;
  }
  if (presence.scene != Scene::Joining) {
    presence.server.clear();
  }
  presence.scene = Scene::Playing;
  presence.since = unix_now();
}

std::string user_id() {
  return current_user_id();
}

std::string authorize(unsigned timeout_ms) {
  std::unique_lock<std::mutex> lock(state_mutex);
  if (!started || !status.available) {
    return {};
  }
  // One request at a time: a second asker waits for the same answer.
  if (!authorization.wanted) {
    authorization = Authorization{};
    authorization.wanted = true;
  }
  const bool answered = authorization_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                                       [] { return authorization.done; });
  if (!answered) {
    // The player did not answer in time: a late answer is dropped.
    authorization.wanted = false;
    authorization.sent = false;
    return {};
  }
  return authorization.code;
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  if (operation == "discordPresence") {
    std::lock_guard<std::mutex> lock(state_mutex);
    presence.script.name = text(message, "name");
    presence.script.state = text(message, "state");
    presence.script.large_image = text(message, "largeImage");
    presence.script.large_text = text(message, "largeText");
    presence.script.small_image = text(message, "smallImage");
    presence.script.small_text = text(message, "smallText");
    presence.script.button_label = text(message, "buttonLabel");
    presence.script.button_url = text(message, "buttonUrl");
    return true;
  }
  // The server's player count and places (its reserved client event kcds:playerCount).
  if (operation == "discordPlayers") {
    const sdk::Value* players = kcd2s::integration::script_field(message, "players");
    const sdk::Value* places = kcd2s::integration::script_field(message, "max");
    std::lock_guard<std::mutex> lock(state_mutex);
    const auto whole = [](const sdk::Value* value) {
      if (value && value->tag() == sdk::Tag::Int) {
        return int(value->as_int());
      }
      if (value && value->tag() == sdk::Tag::Double && value->as_double() >= 0 &&
          value->as_double() < 100000) {
        return int(value->as_double());
      }
      return 0;
    };
    presence.players = whole(players);
    presence.max_players = whole(places);
    return true;
  }
  if (operation == "discordClear") {
    std::lock_guard<std::mutex> lock(state_mutex);
    presence.script = ScriptPresence{};
    return true;
  }
  return false;
}

void pump(const void* session, const RuntimeSender& send) {
  Status now;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    now = status;
  }
  if (session == told_session && told_status && *told_status == now) {
    return;
  }
  sdk::Value::Map message{{"op", sdk::Value(std::string("discordStatus"))},
                          {"available", sdk::Value(now.available)},
                          {"userId", sdk::Value(now.user_id)}};
  if (send(sdk::Value(std::move(message)))) {
    told_session = session;
    told_status = now;
  }
}

}  // namespace kcds::runtime::discord
