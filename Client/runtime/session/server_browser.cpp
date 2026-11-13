#include "server_browser.hpp"
#include "address_cards.hpp"
#include "server_pings.hpp"
#include "../voice/voice_client.hpp"

#include "../bugreport/bug_report.hpp"
#include "../camera/third_person.hpp"
#include "../chat/chat_overlay.hpp"
#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../discord/discord_presence.hpp"
#include "../identity/discord_login.hpp"
#include "../identity/epic_licence.hpp"
#include "../identity/hardware_id.hpp"
#include "../identity/licence_pass.hpp"
#include "../identity/nickname.hpp"
#include "../game_folder/game_folder.hpp"
#include "../kcd1/kcd1_map.hpp"
#include "../maps/custom_maps.hpp"
#include "../maps/dlc_levels.hpp"
#include "../startup_tables/startup_tables.hpp"
#include "../web/resource_web.hpp"
#include "../architect/local_mode.hpp"
#include "../branding/loading_screen.hpp"
#include "../studio/studio_scene.hpp"
#include "server_address.hpp"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "kcd2s/platform/http_win.hpp"
#include "kcd2s/platform/menu_session.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "kcd2s/wire/packet.hpp"
#include "kcds/launcher/json.hpp"
#include "kcds/menu/compositor.hpp"
#include "kcds/menu_native/main_menu.hpp"

namespace kcds::game_client {

namespace integration = kcd2s::integration;
namespace native_chat = kcd2s::native_chat;
namespace platform = kcd2s::platform;
namespace transport = kcd2s::transport;

// ---- In-game server browser -------------------------------------------------------
// The page (Client/ui/compositor) lists the master server's servers; MenuSession answers it. The
// browser runs on its own thread, started as soon as the main-menu hooks exist, so the page shows
// with the native main menu and never waits for the signature scans or the network worker. The
// thread dials the chosen server, reports each join step, and after the join watches the session
// so a lost server brings the page back with the Disconnected popup.
constexpr double kJoinTimeoutSeconds = 20;
// After the join, the session counts as lost after this long without a datagram of the server.
constexpr double kSessionLostSeconds = 10;
// Winsock and the engine hooks are ready (set by initialize), or initialize gave up.
std::atomic<bool> network_ready{false};
// Raised by the ESC menu's "Report a bug"; the browser thread opens the dialog.
std::atomic<bool> bug_report_requested{false};
// A sent report's outcome for the dialog (the sending thread leaves it, the browser thread posts
// it).
std::mutex bug_report_result_mutex;
std::string bug_report_result;
std::atomic<bool> adapter_failed{false};
std::atomic<bool> hooks_installed{false};
std::atomic<bool> game_unsupported{false};
// The player chose ANIM / ASSETS or WORLD ARCHITECT: this game is in that tool now, and the
// browser's page stays down.
std::atomic<bool> tool_started{false};
// The browser thread started again after a tool's Leave: the launcher's server or join link was
// already followed at the game's start and is not joined again.
std::atomic<bool> browser_after_tool{false};

// The main menu's tool in this game: the local mode on (developer mode, the tool's
// variables), the page down, and the offline client the local mode runs its resources in, as a
// start in the tool opens it (network_session.cpp open_network). The engine thread then loads the
// tool's world from the main menu (frame.cpp: the local mode's world config).
void start_tool(bool studio) {
  if (!kcds::runtime::architect::local_mode::start_from_menu(studio)) {
    return;
  }
  tool_started.store(true);
  // KCDS's loading screen covers the game from the press until the tool's editor is drawn: the
  // world and the HUD showed for a second or more before the editor.
  kcds::runtime::branding::loading_screen::show_again(
      [] {
        return !kcds::runtime::architect::local_mode::enabled() ||
               kcds::runtime::architect::local_mode::editor_shown();
      },
      L"Loading Editor");
  kcds::menu::set_visible(false);
  kcds::menu_native::set_hide_main_menu_buttons(true);
  // The tool's editor takes its clicks in the game window: the page that was clicked hands it the
  // foreground back (after a tool's Leave the page kept it, and the next tool's editor got no
  // clicks while the camera still turned).
  if (auto* game_window = static_cast<HWND>(kcd2s::native_chat::window())) {
    SetForegroundWindow(game_window);
  }
  if (!open_client("127.0.0.1:9")) {
    log("MENU tool: the offline client did not open");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    client->set_offline();
  }
  client_ready.store(true);
  log("MENU tool: %s from the main menu",
      studio ? "the Asset/Animation Studio" : "World Architect");
}

std::string master_url() {
  char text[512] = {};
  const DWORD length = GetEnvironmentVariableA("KCDS_MASTER_URL", text, sizeof(text));
  if (length == 0) {
    return "https://master.kcdsynced.com";
  }
  if (length >= sizeof(text) || std::string(text) == "off") {
    return {};
  }
  std::string url(text);
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}
// The last master request's failure, for the page's "Show details" (browser thread only).
std::string last_master_error;
platform::MasterClient browser_master() {
  auto get = [](const std::string& url) -> std::optional<std::string> {
    // HTTPS; plain HTTP only to a loopback master during development.
    const auto result = platform::http_win::request("GET", url, "", 5000, 1024 * 1024);
    last_master_error = !result.error.empty()  ? result.error
                        : result.status != 200 ? "HTTP " + std::to_string(result.status)
                                               : std::string();
    if (!result.error.empty() || result.status != 200) {
      log("MENU master list unavailable url=%s http=%d %s", url.c_str(), result.status,
          result.error.c_str());
      return std::nullopt;
    }
    return result.body;
  };
  return platform::MasterClient(master_url(), get, nullptr);
}
void post_to_page(const std::vector<std::string>& messages) {
  for (const auto& message : messages) {
    kcds::menu::post_to_page(message);
  }
}
// The game's version from KingdomCome.exe's version resource ("1.5.6"), for the browser footer.
std::string game_version() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(exe, &ignored);
  if (size == 0) {
    return {};
  }
  std::vector<std::uint8_t> data(size);
  struct Translation {
    WORD language;
    WORD code_page;
  }* translation = nullptr;
  UINT length = 0;
  if (!GetFileVersionInfoW(exe, 0, size, data.data()) ||
      !VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                      reinterpret_cast<void**>(&translation), &length) ||
      length < sizeof(Translation)) {
    return {};
  }
  wchar_t key[64];
  swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductVersion", translation->language,
             translation->code_page);
  wchar_t* text = nullptr;
  if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&text), &length) || !text) {
    return {};
  }
  std::string version;
  for (const wchar_t* c = text; *c && version.size() < 32; ++c) {
    version.push_back(char(*c < 128 ? *c : '?'));
  }
  // "1.5.6.0" -> "1.5.6"
  while (version.size() > 2 && version.compare(version.size() - 2, 2, ".0") == 0 &&
         std::count(version.begin(), version.end(), '.') > 2) {
    version.resize(version.size() - 2);
  }
  return version;
}
// Quit (and Update) close the game the way its window's close button does. Update first starts
// our launcher, which downloads the release and starts the game again.
void close_game(bool start_launcher) {
  if (start_launcher) {
    const std::wstring launcher = module_directory() + L"kcds-launcher.exe";
    if (GetFileAttributesW(launcher.c_str()) != INVALID_FILE_ATTRIBUTES) {
      const auto started = reinterpret_cast<INT_PTR>(ShellExecuteW(
          nullptr, L"open", launcher.c_str(), nullptr, module_directory().c_str(), SW_SHOWNORMAL));
      log("MENU update: launcher started=%d", int(started > 32));
    } else {
      log("MENU update: kcds-launcher.exe not found next to the client");
    }
  }
  // The game window ignores WM_CLOSE; the engine thread quits like the game's own Exit, also when
  // the player chose to quit before the adapter finished its setup.
  quit_requested.store(true);
  ULONGLONG none = 0;
  quit_requested_at.compare_exchange_strong(none, GetTickCount64());
  log("MENU quit requested%s", native_env ? "" : " (waiting for the native exit)");
}
bool handle_game_message(const kcd2s::sdk::Value& message) {
  if (integration::script_operation(message) != "gameQuit") {
    return false;
  }
  log("RESOURCE Game.quit: the game closes");
  close_game(false);
  return true;
}
// A quit that the engine thread could not serve in time ends the process.
void quit_fallback_if_late() {
  const ULONGLONG at = quit_requested_at.load();
  if (at == 0 || GetTickCount64() - at < kQuitFallbackMs) {
    return;
  }
  log("MENU quit: native exit unavailable after %llu ms; ending the process", kQuitFallbackMs);
  TerminateProcess(GetCurrentProcess(), 0);
}
// The logs a bug report sends: this client's, the launcher's and the game's kcd.log (two folders
// above the game's executable).
std::vector<std::pair<std::string, std::wstring>> bug_report_logs() {
  std::vector<std::pair<std::string, std::wstring>> logs;
  logs.emplace_back("client.log", client_log_path());
  wchar_t local[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
    logs.emplace_back("launcher.log", std::wstring(local) + L"\\KCDS\\logs\\launcher.log");
  }
  const std::filesystem::path root = kcds::runtime::game_folder::root();
  if (!root.empty()) {
    logs.emplace_back("kcd.log", (root / L"kcd.log").wstring());
  }
  return logs;
}

// A bug report message of the page ({"type":"bugReport", text, accept}, "bugReportShown" or
// "bugReportClose"): handled here, true; any other message is the session's.
bool handle_bug_report_message(const std::string& text, const platform::MenuSession& session) {
  if (text.find("\"bugReport") == std::string::npos) {
    return false;
  }
  namespace json = kcds::launcher::json;
  json::Limits limits;
  limits.max_bytes = 16384;
  limits.max_depth = 3;
  json::Value message;
  std::string error;
  if (!json::parse(text, limits, message, error) || !message.is_object()) {
    return false;
  }
  const json::Value& type = message.member("type");
  if (!type.is_string() || (type.string() != "bugReport" && type.string() != "bugReportShown" &&
                            type.string() != "bugReportClose")) {
    return false;
  }
  // The page shows only once it says the dialog is open: a page that cannot open it (an older
  // page) never covers the ESC menu, and the game keeps its input.
  if (type.string() == "bugReportShown") {
    log("BUGREPORT dialog shown");
    page_over_world.store(true);
    kcds::menu::set_visible(true);
    return true;
  }
  if (type.string() == "bugReport") {
    const json::Value& body = message.member("text");
    const json::Value& accept = message.member("accept");
    if (body.is_string() && accept.is_boolean() && accept.boolean()) {
      kcds::runtime::bug_report::Report report;
      report.text = body.string();
      report.nickname = kcds::runtime::identity::local_nickname();
      report.version = KCDS_GAME_CLIENT_VERSION;
      const platform::ServerEntry& target = session.target();
      report.server = target.name == target.address ? target.address
                                                    : target.name + " (" + target.address + ")";
      report.steam_id = kcds::runtime::identity::steam_id();
      report.epic_id = kcds::runtime::identity::epic_account_id();
      report.discord_id = kcds::runtime::discord::user_id();
      report.hardware_id = kcds::runtime::identity::hardware_id();
      report.logs = bug_report_logs();
      // For now the dialog shows the outcome, until it closes.
      const auto leave_result = [](const kcds::runtime::bug_report::Outcome& outcome) {
        std::lock_guard<std::mutex> lock(bug_report_result_mutex);
        bug_report_result = kcds::runtime::bug_report::outcome_message(outcome);
      };
      if (!kcds::runtime::bug_report::send(std::move(report), log, leave_result)) {
        log("BUGREPORT a report is still being sent; this one is dropped");
        kcds::runtime::bug_report::Outcome busy;
        busy.error = "a report is still being sent";
        leave_result(busy);
      }
    }
    return true;
  }
  // Closed: the dialog is gone and the player is back in the ESC menu.
  if (page_over_world.load()) {
    page_over_world.store(false);
    kcds::menu::set_visible(false);
  }
  return true;
}

// Answers the page and handles the player's quit and update choices.
void serve_page(platform::MenuSession& session) {
  quit_fallback_if_late();
  if (bug_report_requested.exchange(false)) {
    log("BUGREPORT dialog requested from the ESC menu");
    post_to_page({std::string("{\"type\":\"bugReport\"}")});
  }
  {
    std::lock_guard<std::mutex> lock(bug_report_result_mutex);
    if (!bug_report_result.empty()) {
      post_to_page({bug_report_result});
      bug_report_result.clear();
    }
  }
  for (const auto& message : kcds::menu::take_page_messages()) {
    if (handle_bug_report_message(message, session)) {
      continue;
    }
    post_to_page(session.handle_page_message(message));
  }
  if (signaled(page_message_event)) {
    char temp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, temp);
    std::ifstream file(std::string(temp) + "kcds-page-" + std::to_string(GetCurrentProcessId()) +
                       ".txt");
    std::string line;
    while (std::getline(file, line)) {
      if (!line.empty()) {
        // A server password stays out of the log.
        if (line.find("\"password\"") == std::string::npos) {
          log("MENU test page message %s", line.c_str());
        } else {
          log("MENU test page message with a password");
        }
        post_to_page(session.handle_page_message(line));
      }
    }
  }
  if (session.take_quit()) {
    close_game(false);
  }
  // Each list the master gives is pinged once; the page gets it again with the pings.
  kcds::runtime::session::pings::start(session.list_generation(), session.server_addresses());
  if (auto measured = kcds::runtime::session::pings::take_results()) {
    log("MENU pinged %zu servers", measured->size());
    post_to_page(session.apply_pings(*measured));
  }
  // The favourites by address: each server's own card, when the page names them and at every
  // refresh.
  kcds::runtime::session::address_cards::start(session.address_generation(),
                                               session.favourite_addresses());
  if (auto cards = kcds::runtime::session::address_cards::take_results()) {
    std::size_t reached = 0;
    for (const auto& card : *cards) {
      reached += card.reached ? 1 : 0;
    }
    log("MENU favourites by address: %zu asked, %zu answered", cards->size(), reached);
    post_to_page({platform::MenuSession::address_servers_message(*cards)});
  }
  // ANIM / ASSETS and WORLD ARCHITECT: this game goes into the tool from its main
  // menu (a second game that loaded the tool through its menu again was
  // wrong). The browser thread ends with it (tool_started).
  const platform::MenuSession::Tool tool = session.take_tool();
  if (tool != platform::MenuSession::Tool::None && !tool_started.load()) {
    start_tool(tool == platform::MenuSession::Tool::Studio);
  }
  // The logs button: the folder with every log of this game (the client's and the launcher's)
  // opens in Explorer, to send with a bug report.
  if (session.take_logs()) {
    const std::wstring folder = logs_directory();
    const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", folder.empty() ? module_directory().c_str() : folder.c_str(), nullptr,
        nullptr, SW_SHOWNORMAL));
    log("MENU logs: the logs folder %s", opened > 32 ? "opened" : "could not be opened");
  }
  // Open website: a listed server's website opens in the player's own web browser, outside the
  // game.
  if (const std::string website = session.take_website(); !website.empty()) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, website.c_str(), -1, nullptr, 0);
    std::wstring address(length > 0 ? std::size_t(length) : 0, L'\0');
    if (length > 0) {
      MultiByteToWideChar(CP_UTF8, 0, website.c_str(), -1, address.data(), length);
      address.resize(std::size_t(length - 1));
    }
    const auto opened = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", address.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    log("MENU website: %s %s", website.c_str(), opened > 32 ? "opened" : "could not be opened");
  }
  // The gear: the page steps aside and the game's own settings open over the main menu.
  if (session.take_settings()) {
    if (kcds::menu_native::open_settings()) {
      log("MENU settings: the game's settings open");
      kcds::menu::set_visible(false);
    } else {
      log("MENU settings: the main menu gave no Settings button yet");
    }
  }
  if (session.take_update()) {
    close_game(true);
  }
  // Verify Discord: the sign-in runs in the browser; the page hears how it went.
  if (session.take_verify_discord()) {
    log("MENU Verify Discord pressed");
    if (kcds::runtime::identity::start_discord_login(log)) {
      post_to_page({platform::MenuSession::discord_message("waiting")});
    }
  }
  if (const auto login = kcds::runtime::identity::take_discord_login_result()) {
    const bool verified = login->state == kcds::runtime::identity::DiscordLogin::Verified;
    post_to_page(
        {platform::MenuSession::discord_message(verified ? "verified" : "failed", login->message)});
  }
  // Closing the Disconnected popup leaves the player in the world; the main-menu page cannot be
  // dismissed.
  if (session.take_dismiss() && page_over_world.load()) {
    kcds::menu::set_visible(false);
  }
}
// Shows the page over the native main menu while the player chooses. The game's own first-time
// pages (EULA, telemetry) come first: while one is open the page and its input capture are off,
// so the player answers them with mouse and keyboard; nothing is accepted for the player.
void show_page_with_main_menu() {
  // The game's settings page, opened by the gear, is one of the game's own pages too: the page
  // comes back once Back or Esc returns to the main menu.
  const bool native_pages =
      kcds::menu_native::first_time_overlay_open() || kcds::menu_native::settings_open();
  // A server's loading page (a login before the world) is the player's screen while it is up:
  // the main menu built under it must not bring the browser's window back over it, as it did
  // when a player joined again from the main menu.
  const bool server_page = kcds::runtime::web::loading_page_up();
  const bool menu_up = kcds::menu_native::main_menu_built() && !native_pages && !server_page;
  if (menu_up && kcds::menu::page_loaded() && !kcds::menu::visible()) {
    kcds::menu::set_visible(true);
  } else if ((native_pages || server_page) && kcds::menu::visible()) {
    kcds::menu::set_visible(false);
  }
}
// The joined server's name as the player's Discord activity shows it: "" for a direct join by
// address, never the address itself.
std::string discord_server_name(const platform::MenuSession& session) {
  const platform::ServerEntry& target = session.target();
  return target.name == target.address ? std::string() : target.name;
}
// Resolves the address and creates the KSTP client with its resource runtime.
std::optional<platform::JoinFailure> open_session(const std::string& address,
                                                  const std::string& listing = {},
                                                  const std::string& password = {},
                                                  bool kcd1 = false) {
  for (int waited = 0; !network_ready.load(); waited += 50) {
    if (adapter_failed.load() && game_unsupported.load()) {
      return platform::JoinFailure::GameVersion;
    }
    if (adapter_failed.load() || waited > 60000) {
      return platform::JoinFailure::Internal;
    }
    Sleep(50);
  }
  std::string server_ip;
  std::uint16_t server_port = 0;
  // The next start loads this server's own perks (startup_tables/startup_tables.hpp).
  kcds::runtime::startup_tables::joining(address);
  if (!resolve_server_address(address, server_ip, server_port)) {
    return platform::JoinFailure::Address;
  }
  return open_client(server_ip + ":" + std::to_string(server_port), listing, password, kcd1)
             ? std::nullopt
             : std::optional<platform::JoinFailure>(platform::JoinFailure::Internal);
}
void close_session() {
  std::unique_ptr<local::Client> abandoned;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      kcds::runtime::voice::session_ended();
      client->disconnect();
    }
    abandoned = std::move(client);
  }
  abandoned.reset();
  // The client resources stopped with the session: their third-person view goes too.
  kcds::runtime::camera::third_person::forget_script_view();
}
// Whether this game has the level a server runs: its Data\Levels\<level>\level.pak
// next to the game's Bin folder. A DLC level is there only with the DLC. A name that is not a plain
// lowercase word is never looked up.
bool game_has_level(const std::string& level) {
  if (level.empty() || level.size() > 64) {
    return false;
  }
  for (const char c : level) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  // A World Architect custom map is not in the game's folder: the game lists it from the KCDS
  // maps folder.
  if (kcds::runtime::maps::custom::is_custom(level)) {
    return kcds::runtime::maps::custom::listed(level);
  }
  // The KCD1 map is built from this PC's own copy of Kingdom Come: Deliverance.
  if (kcds::runtime::kcd1::is_level(level)) {
    return kcds::runtime::kcd1::listed();
  }
  // A DLC's level: its files come with every copy, the DLC only with the account's Steam licence.
  if (!kcds::runtime::maps::dlc::owned(level)) {
    return false;
  }
  const std::filesystem::path game = kcds::runtime::game_folder::root();
  if (game.empty()) {
    // Without the game's folder the join goes on; the engine reports a level it cannot load.
    return true;
  }
  std::error_code error;
  return std::filesystem::exists(
      game / L"Data" / L"Levels" / std::wstring(level.begin(), level.end()) / L"level.pak", error);
}
// Follows one join attempt, reporting its steps to the page, until the native world request.
// Returns false (session closed) on a refusal, timeout or cancel. A server with the Kingdom Come:
// Deliverance map that refused a pass without its check (and `kcd1_pass` was false) sets
// `needs_kcd1` instead of failing: the caller joins again with that check.
bool follow_join(platform::MenuSession& session, bool kcd1_pass = false,
                 bool* needs_kcd1 = nullptr) {
  using platform::JoinFailure;
  using platform::JoinStep;
  double deadline = local::clock_seconds() + kJoinTimeoutSeconds;
  // The last queue status the page was given.
  std::optional<transport::WaitStatus> queue_shown;
  // The server sends a refusal's reason just before the refusal; if the refusal overtook it,
  // wait this long for the reason before showing the failure.
  constexpr double kRefusalTextGraceSeconds = 0.3;
  double rejected_at = -1.0;
  // A custom map this game does not list yet may come with the server's resources: after they
  // are ready, it has this long to arrive before it is called missing.
  constexpr double kCustomMapWaitSeconds = 20.0;
  double custom_map_wait_from = -1.0;
  // Resource download progress goes to the page when it changed, at most every 0.2 s.
  constexpr double kProgressIntervalSeconds = 0.2;
  double progress_posted_at = -1.0;
  std::int64_t progress_posted_bytes = -1;
  std::int64_t progress_posted_resources = -1;
  JoinStep step = JoinStep::Reaching;
  auto advance = [&](JoinStep next) {
    if (int(next) > int(step)) {
      step = next;
      post_to_page(session.join_progress(step));
      log("MENU join step %d", int(step));
    }
  };
  for (;;) {
    bool heard = false;
    bool established = false;
    bool has_world = false;
    std::string world_level;
    bool resources = false;
    std::optional<transport::RefusalReason> refusal;
    std::string server_message;
    std::string master_refusal;
    bool master_refusal_banned = false;
    std::optional<local::Client::ResourceProgress> progress;
    std::optional<transport::WaitStatus> waiting;
    {
      std::lock_guard<std::mutex> lock(network_mutex);
      if (!client_ready.load() || !network_worker_running.load()) {
        client->tick(local::clock_seconds());
        pump_web_views();
      }
      heard = client->heard_from_server();
      established = client->established();
      has_world = client->peer().world_config().has_value();
      if (has_world) {
        world_level = client->peer().world_config()->map;
      }
      resources = client->resources_ready();
      refusal = client->last_refusal();
      server_message = client->last_refusal_text();
      master_refusal = client->licence_refusal();
      master_refusal_banned = client->licence_refusal_banned();
      progress = client->resource_progress();
      waiting = client->wait_status();
    }
    // A full server keeps the joiner in its queue: the page shows the place, the time
    // and the server's fields, and the join does not time out while the server answers.
    if (waiting && !established) {
      deadline = local::clock_seconds() + kJoinTimeoutSeconds;
      if (waiting->place > 0 && (!queue_shown || !(*queue_shown == *waiting))) {
        std::vector<platform::QueueField> fields;
        for (const transport::WaitField& field : waiting->fields) {
          fields.push_back({field.label, field.value});
        }
        post_to_page(
            session.queue_update(int(waiting->place), int(waiting->waited_seconds), fields));
        queue_shown = waiting;
      }
    }
    if (heard) {
      advance(JoinStep::Admission);
      // The server answered without refusing the build: the version is checked, and its
      // requirements (licence, password, bans, Discord, whitelists) are being checked now.
      if (refusal != transport::RefusalReason::OtherVersion) {
        advance(JoinStep::Requirements);
      }
    }
    if (established) {
      advance(JoinStep::Resources);
    }
    // A level this game does not have (a DLC level): the world is never started.
    const bool level_here = has_world && game_has_level(world_level);
    bool missing_level = has_world && !level_here;
    if (missing_level && kcds::runtime::maps::custom::is_custom(world_level)) {
      // A custom map may still be on its way with the server's resources.
      const double now = local::clock_seconds();
      if (resources && custom_map_wait_from < 0) {
        custom_map_wait_from = now;
      }
      missing_level = resources && now - custom_map_wait_from > kCustomMapWaitSeconds;
      // The join does not time out while the map may still come.
      if (custom_map_wait_from >= 0) {
        deadline = std::max(deadline, custom_map_wait_from + kCustomMapWaitSeconds + 1.0);
      }
    }
    if (level_here && !client_ready.exchange(true)) {
      // The engine thread starts the server's world from here on (frame()).
      log("MENU server world received");
    }
    if (progress && step == JoinStep::Resources) {
      const double now = local::clock_seconds();
      const bool changed = progress->bytes_done != progress_posted_bytes ||
                           progress->resources_done != progress_posted_resources;
      const bool finished = progress->bytes_done == progress->bytes_total;
      if (changed && (finished || now - progress_posted_at >= kProgressIntervalSeconds)) {
        post_to_page(session.resource_progress(progress->resources_done, progress->resources_total,
                                               progress->bytes_done, progress->bytes_total));
        progress_posted_at = now;
        progress_posted_bytes = progress->bytes_done;
        progress_posted_resources = progress->resources_done;
      }
    }
    if (has_world && resources) {
      advance(JoinStep::World);
    }
    if (world_requested.load()) {
      post_to_page(session.joined());
      log("MENU joined; native world requested");
      return true;
    }
    serve_page(session);
    std::optional<JoinFailure> failure;
    std::string detail;
    if (missing_level) {
      failure =
          kcds::runtime::kcd1::is_level(world_level) ? JoinFailure::Kcd1Level : JoinFailure::Level;
      detail = "level " + world_level;
    } else if (!master_refusal.empty()) {
      // The Master's central ban list refused this player, or this copy could not be
      // proven an original one: no server admits them.
      failure = master_refusal_banned ? JoinFailure::Banned : JoinFailure::Licence;
      server_message = master_refusal;
    } else if (refusal == transport::RefusalReason::ServerFull) {
      failure = JoinFailure::Full;
    } else if (refusal == transport::RefusalReason::Kcd1Required && !kcd1_pass && needs_kcd1) {
      // The server has the Kingdom Come: Deliverance map: join again with a pass the Master
      // checked for that game.
      log("MENU server has the Kingdom Come: Deliverance map: joining again with that check");
      *needs_kcd1 = true;
      close_session();
      return false;
    } else if (refusal == transport::RefusalReason::Rejected ||
               refusal == transport::RefusalReason::PasswordRequired ||
               refusal == transport::RefusalReason::Banned ||
               refusal == transport::RefusalReason::DiscordRequired ||
               refusal == transport::RefusalReason::Kcd1Required) {
      // The server's reason may trail its refusal a moment ("Wrong password." for a password).
      const double now = local::clock_seconds();
      if (rejected_at < 0) {
        rejected_at = now;
      }
      if (!server_message.empty() || now - rejected_at > kRefusalTextGraceSeconds) {
        if (refusal == transport::RefusalReason::PasswordRequired) {
          failure = JoinFailure::Password;
        } else if (refusal == transport::RefusalReason::Banned) {
          failure = JoinFailure::ServerBanned;
        } else if (refusal == transport::RefusalReason::DiscordRequired) {
          failure = JoinFailure::Discord;
        } else {
          failure = JoinFailure::Rejected;
        }
      }
    } else if (refusal == transport::RefusalReason::AdmissionTimeout) {
      failure = JoinFailure::AdmissionTimeout;
    } else if (refusal == transport::RefusalReason::OtherVersion) {
      // The server runs another KCDS version; its text names it and follows the
      // refusal.
      const double now = local::clock_seconds();
      if (rejected_at < 0) {
        rejected_at = now;
      }
      if (!server_message.empty() || now - rejected_at > kRefusalTextGraceSeconds) {
        log("MENU join refused: server runs another version");
        close_session();
        post_to_page(session.version_refused(transport::refused_version(server_message)));
        return false;
      }
    } else if (refusal) {
      failure = JoinFailure::Rejected;
      detail = "refusal " + std::to_string(int(*refusal));
    }
    if (!failure && !client_ready.load()) {
      if (session.take_cancel()) {
        log("MENU join cancelled");
        close_session();
        return false;
      }
      if (local::clock_seconds() > deadline) {
        failure = JoinFailure::Timeout;
      }
    }
    if (failure) {
      log("MENU join failed kind=%d step=%d server_message=%u", int(*failure), int(step),
          unsigned(server_message.size()));
      close_session();
      post_to_page(session.join_failed(*failure, step, detail, server_message));
      return false;
    }
    Sleep(10);
  }
}
// After the join: a server that stops answering brings the page back over the world. Returns once
// the player left the world back to the main menu; true when they asked to join again.
bool watch_session(platform::MenuSession& session) {
  bool lost = false;
  bool reconnect = false;
  for (;;) {
    if (world_left.exchange(false)) {
      page_over_world.store(false);
      return reconnect;
    }
    double silent = 0;
    std::optional<integration::SessionEnd> ended;
    {
      std::lock_guard<std::mutex> lock(network_mutex);
      if (client) {
        if (!network_worker_running.load()) {
          client->tick(local::clock_seconds());
          pump_web_views();
        }
        silent = client->seconds_since_server(local::clock_seconds());
        ended = client->peer().take_session_end();
      }
    }
    if (!lost && ended) {
      // The server ended the session and said why (shut down, kicked): the page takes
      // the player back to the main menu at once with that message.
      lost = true;
      std::string message = ended->reason;
      const char* why = "closed";
      if (ended->kind == integration::SessionEndKind::Kicked) {
        why = "kicked";
        message = message.empty() ? "You were kicked from the server."
                                  : "You were kicked from the server: " + message;
      } else if (ended->kind == integration::SessionEndKind::Banned) {
        // The server's own bans (Player.ban, Bans): their text names the reason.
        why = "banned";
        if (message.empty()) {
          message = "You are banned from this server.";
        }
      } else if (ended->kind == integration::SessionEndKind::Left ||
                 ended->kind == integration::SessionEndKind::Quit) {
        // A resource let the player go (Player.disconnect): its text as it is.
        why = ended->kind == integration::SessionEndKind::Quit ? "quit" : "left";
        if (message.empty()) {
          message = "You left the server.";
        }
      } else if (message.empty()) {
        message = "The server was shut down.";
      }
      log("MENU session ended by the server (%s)", why);
      // ...and with { quit: true } closes the game as the menu's Quit does.
      if (ended->kind == integration::SessionEndKind::Quit) {
        close_game(false);
      }
      page_over_world.store(true);
      post_to_page(session.disconnected(message, why));
      kcds::runtime::web::forget_loading_page();
      kcds::menu::set_visible(true);
    }
    if (!lost && silent > kSessionLostSeconds) {
      lost = true;
      log("MENU session lost: no datagram from the server for %.0f s", silent);
      page_over_world.store(true);
      post_to_page(session.disconnected("The server stopped answering " +
                                        std::to_string(int(silent)) + " seconds ago."));
      kcds::runtime::web::forget_loading_page();
      kcds::menu::set_visible(true);
    }
    serve_page(session);
    const auto leave = session.take_leave();
    if (leave != platform::MenuSession::Leave::None && !leave_requested.load()) {
      reconnect = leave == platform::MenuSession::Leave::Reconnect;
      log("MENU leave requested (%s)", reconnect ? "reconnect" : "back to list");
      // The page hides with the world; it shows again over the rebuilt main menu.
      kcds::menu::set_visible(false);
      leave_requested.store(true);
    }
    // The player closed the popup to stay in the (offline) world.
    if (page_over_world.load() && !kcds::menu::visible() && !leave_requested.load()) {
      page_over_world.store(false);
    }
    Sleep(100);
  }
}
DWORD WINAPI browser_thread(void*) {
  platform::MenuSession session(browser_master(), int(wire::kProtocolVersion),
                                KCDS_GAME_CLIENT_VERSION, game_version());
  session.set_error_detail([] {
    return last_master_error.empty() ? std::string() : master_url() + " " + last_master_error;
  });
  // A join link (kcds): joined as soon as the page shows.
  std::string link_address;
  std::string link_server;
  launcher_join_link(link_address, link_server);
  // The launcher's server (its configuration or --connect) is joined the same way.
  const std::string launcher_address = launcher_server_address();
  if (link_address.empty() && link_server.empty()) {
    link_address = launcher_address;
  }
  if (browser_after_tool.load()) {
    link_address.clear();
    link_server.clear();
    // The page's messages waited while the tool ran. A tool click among them is stale (a double
    // click on the tool's button): it started that tool again by itself after its Leave, and the
    // tool the player then chose opened only on the next click. Stale tool clicks are dropped;
    // anything else the page sent is handled as usual.
    std::size_t dropped = 0;
    for (const auto& message : kcds::menu::take_page_messages()) {
      if (message.find("\"tool\"") != std::string::npos &&
          message.find("\"type\":\"tool\"") != std::string::npos) {
        ++dropped;
        continue;
      }
      post_to_page(session.handle_page_message(message));
    }
    if (dropped > 0) {
      log("MENU %zu tool click(s) from before the tool's Leave dropped", dropped);
    }
  }
  if (!link_address.empty() || !link_server.empty()) {
    log("MENU join link to %s",
        link_address.empty() ? ("server " + link_server).c_str() : link_address.c_str());
    session.set_join_link(link_address, link_server);
  }
  for (;;) {
    if (kcds::menu::failed()) {
      // Without the page the player cannot choose: the launcher's server, else the local one.
      const std::string fallback = launcher_address.empty() ? "127.0.0.1:7777" : launcher_address;
      log("MENU unavailable; joining %s", fallback.c_str());
      kcds::menu_native::set_hide_main_menu_buttons(false);
      if (!open_session(fallback)) {
        client_ready.store(true);
      }
      return 0;
    }
    show_page_with_main_menu();
    if (kcds::menu_native::main_menu_built()) {
      kcds::runtime::discord::set_scene(kcds::runtime::discord::Scene::Menu);
    }
    serve_page(session);
    if (tool_started.load()) {
      // The game is in a tool now (World Architect's local mode): no list, no joins.
      log("MENU server browser ended: a tool started");
      return 0;
    }
    // A join starts only over the built main menu: after a world was left the game loads the
    // main-menu level first.
    if (!kcds::menu_native::main_menu_built()) {
      Sleep(20);
      continue;
    }
    if (auto target = session.take_connect_target()) {
      log("MENU join requested %s", target->c_str());
      const std::string server_name = discord_server_name(session);
      kcds::runtime::discord::set_scene(kcds::runtime::discord::Scene::Joining, server_name);
      if (const auto failure =
              open_session(*target, session.target().id, session.join_password())) {
        post_to_page(session.join_failed(*failure, platform::JoinStep::Reaching, *target));
        continue;
      }
      bool needs_kcd1 = false;
      bool joined = follow_join(session, false, &needs_kcd1);
      if (!joined && needs_kcd1) {
        // Once more, with the Master's check of Kingdom Come: Deliverance in the pass.
        if (const auto failure =
                open_session(*target, session.target().id, session.join_password(), true)) {
          post_to_page(session.join_failed(*failure, platform::JoinStep::Reaching, *target));
          continue;
        }
        joined = follow_join(session, true);
      }
      if (joined) {
        kcds::runtime::discord::set_scene(kcds::runtime::discord::Scene::Playing, server_name);
        const bool reconnect = watch_session(session);
        post_to_page(session.returned_to_list());
        if (reconnect) {
          post_to_page(session.reconnect());
        }
      }
      continue;
    }
    Sleep(20);
  }
}

void request_bug_report() {
  bug_report_requested.store(true);
}

// Alt+F4 and the window's close button quit as the menu's Quit does, with or
// without the server browser.
void quit_on_window_close() {
  native_chat::set_close_handler([]() {
    log("MENU quit by the window's close command");
    close_game(false);
  });
}
// Starts the server browser: native buttons hidden, page compositor in the frame hook, and the
// browser thread.
void start_browser() {
  kcds::menu_native::set_hide_main_menu_buttons(true);
  // The page is a web view: the browser starts with the main menu.
  kcds::runtime::web::start(module_directory(), log);
  kcds::menu::Options options;
  options.log = log;
  // The page is painted by the frame hooks: install them now, not after the adapter's signature
  // scans, or the main menu shows for seconds without the page.
  if (!native_chat::install(log)) {
    log("MENU frame hooks unavailable; the page cannot be painted");
  }
  if (!kcds::menu::start(options)) {
    log("MENU page thread failed to start");
  }
  const HANDLE thread = CreateThread(nullptr, 0, browser_thread, nullptr, 0, nullptr);
  if (thread) {
    CloseHandle(thread);
  }
  log("MENU server browser started master=%s", master_url().c_str());
}
void leave_tool() {
  // Asked before the local mode forgets which tool this was.
  const bool studio = kcds::runtime::studio::requested();
  if (!kcds::runtime::architect::local_mode::stop_from_menu()) {
    // Started in the tool (the development launcher): no server list to go back to.
    log("MENU tool Leave: the game started in the tool, so it closes");
    close_game(false);
    return;
  }
  if (!tool_started.exchange(false)) {
    return;
  }
  if (studio) {
    // The studio's console variables go back to the game's, before the next world, and a next
    // studio world is set up anew.
    kcds::runtime::studio::leave();
  }
  log("MENU tool Leave: back to the main menu and the server list");
  // The world goes as a server's does (leave_world.cpp); the page shows over the rebuilt main
  // menu, as after leaving a server.
  leave_requested.store(true);
  browser_after_tool.store(true);
  const HANDLE thread = CreateThread(nullptr, 0, browser_thread, nullptr, 0, nullptr);
  if (thread) {
    CloseHandle(thread);
  }
}
}  // namespace kcds::game_client
