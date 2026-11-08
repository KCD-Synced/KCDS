#include "kcd2s/platform/menu_session.hpp"

#include <algorithm>
#include <cstdint>

#include "kcds/launcher/json.hpp"
#include "kcds/launcher/server_address.hpp"

namespace kcd2s::platform {

namespace {

namespace json = kcds::launcher::json;

// Page messages are tiny; anything larger is not ours.
constexpr std::size_t kMaxPageMessageBytes = 4096;

std::string string_member(const json::Value& message, const char* key) {
  const json::Value& value = message.member(key);
  return value.is_string() ? value.string() : std::string();
}

void append_field(std::string& out, const char* key, const std::string& value) {
  out += '"';
  out += key;
  out += "\":\"";
  append_json_string(out, value);
  out += '"';
}

// Removes control characters and cuts text to at most limit bytes without splitting a UTF-8
// sequence.
std::string plain_text(const std::string& text, std::size_t limit) {
  std::string out;
  for (char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) {
      continue;
    }
    out.push_back(c);
  }
  if (out.size() <= limit) {
    return out;
  }
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  out.resize(cut);
  return out;
}

const char* failure_kind(JoinFailure kind) {
  switch (kind) {
    case JoinFailure::Timeout:
      return "timeout";
    case JoinFailure::Full:
      return "full";
    case JoinFailure::Rejected:
      return "rejected";
    case JoinFailure::AdmissionTimeout:
      return "admission";
    case JoinFailure::Address:
      return "address";
    case JoinFailure::Banned:
      return "banned";
    case JoinFailure::Password:
      return "password";
    case JoinFailure::Level:
      return "level";
    case JoinFailure::Licence:
      return "licence";
    case JoinFailure::GameVersion:
      return "gameVersion";
    case JoinFailure::ServerBanned:
      return "serverBanned";
    case JoinFailure::Discord:
      return "discord";
    case JoinFailure::Internal:
    default:
      return "internal";
  }
}

// The body sentence of the ConnectFailed popup: what happened and what the player can do.
const char* failure_reason(JoinFailure kind) {
  switch (kind) {
    case JoinFailure::Timeout:
      return "The server did not answer in time. It may be restarting, or your firewall is "
             "blocking the game.";
    case JoinFailure::Full:
      return "Every place on this server is taken. Try again later.";
    case JoinFailure::Rejected:
      return "The server turned you away: one of its scripts refused the connection.";
    case JoinFailure::AdmissionTimeout:
      return "The server's scripts did not let you in in time. Try again in a moment.";
    case JoinFailure::Address:
      return "The address could not be found. Check the host name and the port.";
    case JoinFailure::Banned:
      return "You are banned from every KCDS server, so no server can let you in. To appeal, "
             "write to the KCDS team on our Discord with the ban id below.";
    case JoinFailure::Password:
      return "This server asks for a password. Enter it to join.";
    case JoinFailure::Level:
      return "This server runs a part of the world your game does not have: it comes with a DLC. "
             "Install the DLC to join.";
    case JoinFailure::Licence:
      return "KCDS lets you join only with an original copy of Kingdom Come: Deliverance II, and "
             "this one could not be proven one.";
    case JoinFailure::GameVersion:
      return "This KCDS release does not support your game's version: game functions it needs "
             "were not found. Update the game and KCDS; the client log names what is missing.";
    case JoinFailure::ServerBanned:
      return "This server has banned you, so it does not let you in. Other servers still do.";
    case JoinFailure::Discord:
      return "This server lets in only players who verified their Discord account. Verify yours "
             "in your browser, then join again.";
    case JoinFailure::Internal:
    default:
      return "The game client could not start the connection. See the client log.";
  }
}

const char* failure_code(JoinFailure kind) {
  switch (kind) {
    case JoinFailure::Timeout:
      return "timed out";
    case JoinFailure::Full:
      return "refused: server full";
    case JoinFailure::Rejected:
      return "refused: playerConnecting";
    case JoinFailure::AdmissionTimeout:
      return "refused: playerConnecting timed out";
    case JoinFailure::Address:
      return "address not resolved";
    case JoinFailure::Banned:
      return "refused: KCDS central ban";
    case JoinFailure::Password:
      return "refused: password";
    case JoinFailure::Level:
      return "missing level";
    case JoinFailure::Licence:
      return "licence not proven";
    case JoinFailure::GameVersion:
      return "unsupported game version";
    case JoinFailure::ServerBanned:
      return "refused: banned by this server";
    case JoinFailure::Discord:
      return "refused: Discord not verified";
    case JoinFailure::Internal:
    default:
      return "client error";
  }
}

// True when version a ("1.2.10") is newer than b, comparing the dot-separated numbers.
bool newer_version(const std::string& a, const std::string& b) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() || j < b.size()) {
    unsigned long x = 0;
    unsigned long y = 0;
    while (i < a.size() && a[i] != '.') {
      x = x * 10 + (a[i] >= '0' && a[i] <= '9' ? unsigned(a[i] - '0') : 0);
      ++i;
    }
    while (j < b.size() && b[j] != '.') {
      y = y * 10 + (b[j] >= '0' && b[j] <= '9' ? unsigned(b[j] - '0') : 0);
      ++j;
    }
    if (x != y) {
      return x > y;
    }
    ++i;
    ++j;
  }
  return false;
}

}  // namespace

std::vector<QueueField> sanitize_queue_fields(const std::vector<QueueField>& fields) {
  std::vector<QueueField> out;
  for (const QueueField& field : fields) {
    if (out.size() >= kMaxQueueFields) {
      break;
    }
    QueueField clean{plain_text(field.label, kMaxQueueLabelBytes),
                     plain_text(field.value, kMaxQueueValueBytes)};
    if (clean.label.empty() && clean.value.empty()) {
      continue;
    }
    out.push_back(std::move(clean));
  }
  return out;
}

MenuSession::MenuSession(MasterClient master, int own_protocol, std::string client_version,
                         std::string game_version)
    : menu_(std::move(master), own_protocol),
      own_protocol_(own_protocol),
      client_version_(std::move(client_version)),
      game_version_(std::move(game_version)) {
}

std::vector<std::string> MenuSession::handle_page_message(const std::string& text) {
  json::Limits limits;
  limits.max_bytes = kMaxPageMessageBytes;
  limits.max_depth = 4;
  json::Value message;
  std::string error;
  if (!json::parse(text, limits, message, error) || !message.is_object()) {
    return {};
  }
  const std::string type = string_member(message, "type");

  if (type == "ready") {
    std::vector<std::string> out{self_message()};
    for (auto& next : refresh()) {
      out.push_back(std::move(next));
    }
    // A join link waits for the page and the list (a listed server is found by its id).
    std::string link;
    if (!link_address_.empty()) {
      link = "{\"type\":\"direct\",";
      append_field(link, "address", link_address_);
      link += '}';
    } else if (!link_server_.empty()) {
      link = "{\"type\":\"connect\",";
      append_field(link, "id", link_server_);
      link += '}';
    }
    link_address_.clear();
    link_server_.clear();
    if (!link.empty()) {
      for (auto& next : handle_page_message(link)) {
        out.push_back(std::move(next));
      }
    }
    return out;
  }
  if (type == "refresh") {
    return refresh();
  }
  if (type == "update") {
    update_ = true;
    return {};
  }
  if (type == "quit") {
    quit_ = true;
    return {};
  }
  if (type == "dismiss") {
    dismiss_ = true;
    return {};
  }
  if (type == "verifyDiscord") {
    verify_discord_ = true;
    return {};
  }
  const bool in_world = menu_.state() == FrontendMenu::State::InGame;
  // The tools also while the list could not load, but never during a join or in a world.
  const bool on_list = menu_.state() != FrontendMenu::State::Connecting && !in_world;
  if (type == "tool" && on_list) {
    const std::string tool = string_member(message, "tool");
    if (tool == "studio") {
      tool_ = Tool::Studio;
    } else if (tool == "architect") {
      tool_ = Tool::Architect;
    }
    return {};
  }
  if (type == "settings" && on_list) {
    settings_ = true;
    return {};
  }
  if (type == "logs" && on_list) {
    logs_ = true;
    return {};
  }
  // Open website: the page names a listed server, and its website from the list is opened (only
  // an http or https address, which the master checked too).
  if (type == "website" && on_list) {
    const std::string id = string_member(message, "id");
    for (const ServerEntry& entry : menu_.browser().all()) {
      const bool web =
          entry.website.rfind("https://", 0) == 0 || entry.website.rfind("http://", 0) == 0;
      if (entry.id == id && web) {
        website_ = entry.website;
      }
    }
    return {};
  }
  if (type == "back" && in_world) {
    leave_ = Leave::BackToList;
    return {};
  }
  if (type == "reconnect" && in_world) {
    leave_ = Leave::Reconnect;
    return {};
  }
  const bool joining = menu_.state() == FrontendMenu::State::Connecting;
  if ((type == "connect" || type == "direct") && !joining) {
    // The server's password, when the player typed one; a longer one is cut as the
    // server would refuse it anyway.
    password_ = string_member(message, "password");
    if (password_.size() > kMaxServerPasswordBytes) {
      password_.clear();
    }
  }
  if (type == "connect" && !joining) {
    const std::string id = string_member(message, "id");
    target_ = ServerEntry{};
    target_.name = id;
    for (const ServerEntry& entry : menu_.browser().all()) {
      if (entry.id == id) {
        target_ = entry;
      }
    }
    const bool other_protocol =
        own_protocol_ != 0 && target_.protocol != 0 && target_.protocol != own_protocol_;
    if (other_protocol) {
      return {update_message()};
    }
    if (!menu_.connect(id)) {
      const std::string reason = menu_.error();
      menu_.back();
      std::string extra = ",\"kind\":\"internal\",\"step\":0,";
      append_field(extra, "reason", reason);
      return {state_message("failed", extra)};
    }
    pending_target_ = menu_.connect_target();
    return {state_message("connecting", ",\"step\":0")};
  }
  if (type == "direct" && !joining) {
    // A host typed without a port (a DNS name or an IP) joins the default game port.
    const std::string address =
        kcds::launcher::server_address::with_default_port(string_member(message, "address"));
    target_ = ServerEntry{};
    target_.name = address;
    target_.address = address;
    std::string host;
    std::uint16_t port = 0;
    if (!kcds::launcher::server_address::parse(address, host, port)) {
      std::string extra = ",\"kind\":\"address\",\"step\":0,";
      append_field(extra, "reason",
                   "Enter a host name or IP, optionally with a port, e.g. play.example.com or "
                   "127.0.0.1:7777.");
      return {state_message("failed", extra)};
    }
    menu_.direct_connect(address);
    pending_target_ = address;
    return {state_message("connecting", ",\"step\":0")};
  }
  if (type == "cancel" && joining) {
    menu_.back();
    pending_target_.reset();
    cancel_ = true;
    return {};
  }
  return {};
}

std::optional<std::string> MenuSession::take_connect_target() {
  auto target = std::move(pending_target_);
  pending_target_.reset();
  return target;
}

bool MenuSession::take_cancel() {
  const bool cancelled = cancel_;
  cancel_ = false;
  return cancelled;
}

bool MenuSession::take_update() {
  const bool requested = update_;
  update_ = false;
  return requested;
}

bool MenuSession::take_dismiss() {
  const bool requested = dismiss_;
  dismiss_ = false;
  return requested;
}

bool MenuSession::take_verify_discord() {
  const bool requested = verify_discord_;
  verify_discord_ = false;
  return requested;
}

bool MenuSession::take_settings() {
  const bool requested = settings_;
  settings_ = false;
  return requested;
}

bool MenuSession::take_logs() {
  const bool requested = logs_;
  logs_ = false;
  return requested;
}

std::string MenuSession::take_website() {
  std::string website;
  website.swap(website_);
  return website;
}

std::string MenuSession::discord_message(const char* state, const std::string& message) {
  std::string out = R"({"type":"discord",)";
  append_field(out, "state", state);
  out += ',';
  append_field(out, "message", plain_text(message, 300));
  return out + "}";
}

MenuSession::Leave MenuSession::take_leave() {
  const Leave requested = leave_;
  leave_ = Leave::None;
  return requested;
}

std::vector<std::string> MenuSession::returned_to_list() {
  menu_.leave_world();
  std::vector<std::string> out{state_message("browsing", "")};
  for (auto& next : refresh()) {
    out.push_back(std::move(next));
  }
  return out;
}

std::vector<std::string> MenuSession::reconnect() {
  if (target_.address.empty() || menu_.state() == FrontendMenu::State::Connecting) {
    return {};
  }
  menu_.direct_connect(target_.address);
  pending_target_ = target_.address;
  return {state_message("connecting", ",\"step\":0")};
}

bool MenuSession::take_quit() {
  const bool requested = quit_;
  quit_ = false;
  return requested;
}

MenuSession::Tool MenuSession::take_tool() {
  const Tool chosen = tool_;
  tool_ = Tool::None;
  return chosen;
}

std::vector<std::string> MenuSession::join_progress(JoinStep step) {
  if (menu_.state() != FrontendMenu::State::Connecting) {
    return {};
  }
  return {state_message("connecting", ",\"step\":" + std::to_string(int(step)))};
}

std::vector<std::string> MenuSession::resource_progress(std::int64_t resources_done,
                                                        std::int64_t resources_total,
                                                        std::int64_t bytes_done,
                                                        std::int64_t bytes_total) {
  if (menu_.state() != FrontendMenu::State::Connecting) {
    return {};
  }
  const std::int64_t resources = std::max<std::int64_t>(resources_total, 0);
  const std::int64_t bytes = std::max<std::int64_t>(bytes_total, 0);
  std::string extra = ",\"step\":" + std::to_string(int(JoinStep::Resources));
  extra += ",\"progress\":{\"resourcesDone\":" +
           std::to_string(std::clamp<std::int64_t>(resources_done, 0, resources));
  extra += ",\"resourcesTotal\":" + std::to_string(resources);
  extra += ",\"bytesDone\":" + std::to_string(std::clamp<std::int64_t>(bytes_done, 0, bytes));
  extra += ",\"bytesTotal\":" + std::to_string(bytes) + "}";
  return {state_message("connecting", extra)};
}

std::vector<std::string> MenuSession::joined() {
  if (menu_.state() != FrontendMenu::State::Connecting) {
    return {};
  }
  menu_.on_connect_result(true);
  return {state_message("ingame", "")};
}

std::vector<std::string> MenuSession::join_failed(JoinFailure kind, JoinStep step,
                                                  const std::string& detail,
                                                  const std::string& server_message) {
  if (menu_.state() != FrontendMenu::State::Connecting) {
    return {};
  }
  menu_.on_connect_result(false);
  menu_.back();
  const bool other_version =
      !target_.version.empty() && !client_version_.empty() && target_.version != client_version_;
  if (kind == JoinFailure::Timeout && other_version) {
    return {update_message()};
  }
  std::string extra = ",";
  append_field(extra, "kind", failure_kind(kind));
  extra += ",\"step\":" + std::to_string(int(step)) + ",";
  append_field(extra, "reason", failure_reason(kind));
  extra += ',';
  std::string code = menu_.connect_target() + " · " + failure_code(kind);
  if (!detail.empty()) {
    code += " · " + plain_text(detail, 160);
  }
  append_field(extra, "code", code);
  const std::string message = plain_text(server_message, kMaxServerMessageBytes);
  if (!message.empty()) {
    extra += ',';
    append_field(extra, "message", message);
  }
  return {state_message("failed", extra)};
}

std::vector<std::string> MenuSession::version_refused(const std::string& server_version) {
  if (menu_.state() != FrontendMenu::State::Connecting) {
    return {};
  }
  menu_.on_connect_result(false);
  menu_.back();
  return {update_message(server_version)};
}

std::vector<std::string> MenuSession::disconnected(const std::string& reason,
                                                   const std::string& cause) {
  std::string extra = ",";
  append_field(extra, "reason", plain_text(reason, 200));
  extra += ',';
  append_field(extra, "cause", plain_text(cause, 16));
  return {state_message("disconnected", extra)};
}

std::vector<std::string> MenuSession::queue_update(int place, int waited_seconds,
                                                   const std::vector<QueueField>& fields) {
  std::string extra = ",\"place\":" + std::to_string(place < 0 ? 0 : place);
  extra += ",\"waited\":" + std::to_string(waited_seconds < 0 ? 0 : waited_seconds);
  extra += ",\"fields\":[";
  bool first = true;
  for (const QueueField& field : sanitize_queue_fields(fields)) {
    if (!first) {
      extra += ',';
    }
    first = false;
    extra += '{';
    append_field(extra, "label", field.label);
    extra += ',';
    append_field(extra, "value", field.value);
    extra += '}';
  }
  extra += ']';
  return {state_message("queue", extra)};
}

std::vector<std::string> MenuSession::refresh() {
  if (menu_.refresh()) {
    return {servers_message("")};
  }
  // An unreachable master leaves an empty list with the reason; the player can still join an
  // address directly.
  std::string reason = menu_.error();
  const std::string detail = error_detail_ ? plain_text(error_detail_(), 200) : std::string();
  if (!detail.empty()) {
    reason += ": " + detail;
  }
  menu_.browser().set_servers({});
  menu_.back();
  return {servers_message(reason)};
}

std::string MenuSession::self_message() const {
  std::string out = "{\"type\":\"self\",\"protocol\":" + std::to_string(own_protocol_) + ",";
  append_field(out, "version", client_version_);
  out += ',';
  append_field(out, "gameVersion", game_version_);
  out += '}';
  return out;
}

std::string MenuSession::servers_message(const std::string& error) const {
  std::string out = "{\"type\":\"servers\",\"servers\":";
  out += encode_server_list(menu_.browser().all());
  if (!error.empty()) {
    out += ',';
    append_field(out, "error", error);
  }
  out += '}';
  return out;
}

std::string MenuSession::state_message(const char* state, const std::string& extra) const {
  std::string out = "{\"type\":\"state\",";
  append_field(out, "state", state);
  out += ",\"server\":{";
  append_field(out, "name", target_.name);
  out += ',';
  append_field(out, "address", target_.address.empty() ? menu_.connect_target() : target_.address);
  out += ",\"players\":" + std::to_string(target_.players);
  out += ",\"maxPlayers\":" + std::to_string(target_.maxPlayers);
  out += ',';
  append_field(out, "image", target_.image);
  out += ',';
  append_field(out, "id", target_.id);
  out += '}';
  out += extra;
  out += '}';
  return out;
}

std::string MenuSession::update_message(const std::string& server_version) const {
  std::string extra = ",";
  append_field(extra, "yours", client_version_);
  extra += ',';
  const std::string& version = server_version.empty() ? target_.version : server_version;
  std::string required = version;
  if (required.empty()) {
    required = target_.protocol > 0 ? "protocol v" + std::to_string(target_.protocol)
                                    : std::string("another version");
  }
  append_field(extra, "required", required);
  // The server is newer when its protocol (or, at an equal protocol, its version) is higher.
  bool newer = target_.protocol > own_protocol_;
  if (!server_version.empty() || target_.protocol == own_protocol_ || target_.protocol == 0) {
    newer = newer_version(version, client_version_);
  }
  extra += newer ? ",\"newer\":true" : ",\"newer\":false";
  return state_message("update", extra);
}

}  // namespace kcd2s::platform
