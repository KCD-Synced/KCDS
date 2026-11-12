#include "licence_pass.hpp"

#include <Windows.h>

#include <charconv>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/platform/http_win.hpp"
#include "kcd2s/platform/master_client.hpp"
#include "../discord/discord_presence.hpp"
#include "discord_login.hpp"
#include "hardware_id.hpp"
#include "xbox_licence.hpp"
#include "epic_licence.hpp"

namespace kcds::runtime::identity {
namespace {

namespace http = kcd2s::platform::http_win;

constexpr int kHttpTimeoutMs = 10000;
constexpr std::uint32_t kGameAppId = 1771300;
constexpr std::uint32_t kMaxOwnershipTicket = 4096;

using SteamInterface = void*(__cdecl*)();
using SteamHandle = std::int32_t(__cdecl*)();
using GetSteamID = std::uint64_t(__cdecl*)(void* user);
using LoggedOn = bool(__cdecl*)(void* user);
using GetAppID = std::uint32_t(__cdecl*)(void* utils);
using GetGenericInterface = void*(__cdecl*)(void* client, std::int32_t user, std::int32_t pipe,
                                            const char* version);
// ISteamAppTicket's only method (its vtable slot 0).
using GetOwnershipTicket = std::uint32_t(__fastcall*)(
    void* tickets, std::uint32_t app, void* buffer, std::uint32_t size, std::uint32_t* app_offset,
    std::uint32_t* steam_id_offset, std::uint32_t* signature_offset, std::uint32_t* signature_size);

HMODULE steam_module() {
  return GetModuleHandleW(L"steam_api64.dll");
}

template <typename Function>
Function steam_function(const char* name) {
  HMODULE steam = steam_module();
  return steam ? reinterpret_cast<Function>(GetProcAddress(steam, name)) : nullptr;
}

void* steam_user() {
  const auto user = steam_function<SteamInterface>("SteamAPI_SteamUser_v023");
  return user ? user() : nullptr;
}

// KCD2's app ownership ticket from the game's own Steam session, in hex; "" when Steam has none.
// Steam signs it with its system key and the Master checks that signature. Steam gives KCD2 no
// session or Web API tickets (both calls answer 0); this ticket it does give.
std::string ownership_ticket(Logger log) {
  void* user = steam_user();
  const auto utils = steam_function<SteamInterface>("SteamAPI_SteamUtils_v010");
  const auto app = steam_function<GetAppID>("SteamAPI_ISteamUtils_GetAppID");
  const auto logged_on = steam_function<LoggedOn>("SteamAPI_ISteamUser_BLoggedOn");
  void* utils_interface = utils ? utils() : nullptr;
  if (!user || !utils_interface || !app || !logged_on || app(utils_interface) != kGameAppId ||
      !logged_on(user)) {
    log("LICENCE no online Steam session for KCD2");
    return {};
  }
  const auto client = steam_function<SteamInterface>("SteamClient");
  const auto user_handle = steam_function<SteamHandle>("SteamAPI_GetHSteamUser");
  const auto pipe = steam_function<SteamHandle>("SteamAPI_GetHSteamPipe");
  const auto generic =
      steam_function<GetGenericInterface>("SteamAPI_ISteamClient_GetISteamGenericInterface");
  void* client_interface = client ? client() : nullptr;
  if (!client_interface || !user_handle || !pipe || !generic) {
    log("LICENCE no Steam client interface in this game");
    return {};
  }
  void* tickets =
      generic(client_interface, user_handle(), pipe(), "STEAMAPPTICKET_INTERFACE_VERSION001");
  if (!tickets) {
    log("LICENCE Steam has no app ticket interface");
    return {};
  }
  const auto get = reinterpret_cast<GetOwnershipTicket>((*reinterpret_cast<void***>(tickets))[0]);
  std::vector<std::uint8_t> buffer(kMaxOwnershipTicket);
  std::uint32_t app_offset = 0;
  std::uint32_t steam_id_offset = 0;
  std::uint32_t signature_offset = 0;
  std::uint32_t signature_size = 0;
  const std::uint32_t size = get(tickets, kGameAppId, buffer.data(), kMaxOwnershipTicket,
                                 &app_offset, &steam_id_offset, &signature_offset, &signature_size);
  if (size == 0 || size > kMaxOwnershipTicket || signature_size == 0) {
    log("LICENCE Steam has no ownership ticket for KCD2");
    return {};
  }
  static const char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(size * 2);
  for (std::uint32_t i = 0; i < size; ++i) {
    hex.push_back(kHex[buffer[i] >> 4]);
    hex.push_back(kHex[buffer[i] & 15]);
  }
  log("LICENCE Steam ownership ticket of %u bytes", size);
  return hex;
}

// The value of a JSON text field in a small, flat answer ("" when missing or not plain).
std::string text_field(const std::string& body, const char* name) {
  const std::string key = std::string("\"") + name + "\":\"";
  const auto start = body.find(key);
  if (start == std::string::npos) {
    return {};
  }
  const auto end = body.find('"', start + key.size());
  return end == std::string::npos ? std::string()
                                  : body.substr(start + key.size(), end - start - key.size());
}

// The value of a JSON string field, its escapes decoded ("" when missing). For the Master's own
// answers, whose text (a ban's reason) may hold quotes, non-ASCII letters and \u escapes.
std::string decoded_text_field(const std::string& body, const char* name) {
  const std::string key = std::string("\"") + name + "\":\"";
  const auto start = body.find(key);
  if (start == std::string::npos) {
    return {};
  }
  std::string text;
  for (std::size_t i = start + key.size(); i < body.size(); ++i) {
    const char c = body[i];
    if (c == '"') {
      return text;
    }
    if (c != '\\') {
      text.push_back(c);
      continue;
    }
    if (++i >= body.size()) {
      break;
    }
    const char escaped = body[i];
    if (escaped == 'n' || escaped == 't' || escaped == 'r') {
      text.push_back(' ');
    } else if (escaped == 'u' && i + 4 < body.size()) {
      unsigned code = 0;
      const auto parsed = std::from_chars(body.data() + i + 1, body.data() + i + 5, code, 16);
      if (parsed.ec != std::errc{} || parsed.ptr != body.data() + i + 5) {
        return {};
      }
      i += 4;
      // A code unit of the Basic Multilingual Plane as UTF-8; a surrogate half shows as '?'.
      if (code >= 0xD800 && code <= 0xDFFF) {
        text.push_back('?');
      } else if (code < 0x80) {
        text.push_back(char(code));
      } else if (code < 0x800) {
        text.push_back(char(0xC0 | (code >> 6)));
        text.push_back(char(0x80 | (code & 0x3F)));
      } else {
        text.push_back(char(0xE0 | (code >> 12)));
        text.push_back(char(0x80 | ((code >> 6) & 0x3F)));
        text.push_back(char(0x80 | (code & 0x3F)));
      }
    } else {
      text.push_back(escaped);
    }
  }
  return {};
}

// The number of a JSON number field ("expires"), or 0.
std::int64_t number_field(const std::string& body, const char* name) {
  const std::string key = std::string("\"") + name + "\":";
  const auto offset = body.find(key);
  std::int64_t value = 0;
  if (offset == std::string::npos) {
    return 0;
  }
  const auto parsed =
      std::from_chars(body.data() + offset + key.size(), body.data() + body.size(), value);
  return parsed.ec == std::errc{} ? value : 0;
}

// What a player the Master's central ban list refuses is told: the reason, the end
// of a temporary ban and the ban's id for an appeal.
std::string ban_text(const std::string& answer) {
  std::string text = "Reason: " + decoded_text_field(answer, "reason");
  if (text.back() != '.' && text.back() != '!' && text.back() != '?') {
    text += '.';
  }
  const std::int64_t expires = number_field(answer, "expires");
  if (expires > 0) {
    const std::time_t until = std::time_t(expires);
    std::tm utc{};
    char date[32] = {};
    if (gmtime_s(&utc, &until) == 0 && std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &utc)) {
      text += std::string(" Until ") + date + " UTC.";
    }
  } else {
    text += " Permanent.";
  }
  const std::string ban = text_field(answer, "ban");
  if (!ban.empty()) {
    text += " Ban id: " + ban;
  }
  return text;
}

// The Master's id of the server at `address`, from its list.
std::string listing_of(const std::string& master_url, const std::string& address) {
  auto get = [](const std::string& url) -> std::optional<std::string> {
    const auto result = http::request("GET", url, "", kHttpTimeoutMs, 1024 * 1024);
    if (!result.error.empty() || result.status != 200) {
      return std::nullopt;
    }
    return result.body;
  };
  const auto servers = kcd2s::platform::MasterClient(master_url, get, nullptr).fetch_servers();
  if (servers) {
    for (const auto& server : *servers) {
      if (server.address == address) {
        return server.id;
      }
    }
  }
  return {};
}

// Encode a request field without letting launcher/store strings alter the JSON.
std::string json_string(const std::string& value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      result.push_back('\\');
      result.push_back(char(c));
    } else if (c < 0x20) {
      result += "\\u00";
      result.push_back(digits[c >> 4]);
      result.push_back(digits[c & 15]);
    } else {
      result.push_back(char(c));
    }
  }
  result.push_back('"');
  return result;
}

bool base64url_text(const std::string& value, std::size_t maximum, bool pass = false) {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  for (char c : value) {
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '-' &&
        c != '_' && !(pass && c == '.')) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string steam_id() {
  void* user = steam_user();
  const auto get = user ? steam_function<GetSteamID>("SteamAPI_ISteamUser_GetSteamID") : nullptr;
  const std::uint64_t id = get ? get(user) : 0;
  return id != 0 ? std::to_string(id) : std::string();
}

std::string licence_pass(const std::string& master_url, const std::string& address,
                         const std::string& listing, const std::string& binding, Logger log,
                         std::int64_t* expires, LicenceRefusal* refusal, bool kcd1) {
  const auto unverified = [refusal](const std::string& text) {
    if (refusal) {
      refusal->banned = false;
      refusal->text = text;
    }
  };
  if (master_url.empty() || binding.size() != 16) {
    return {};
  }
  // The pass's audience: the server's listing, or for a server the Master does not list (a
  // private, LAN or local one) the address this game dials, which that server checks as its own
  std::string server = listing.empty() ? listing_of(master_url, address) : listing;
  if (server.empty()) {
    server = "addr:" + address;
    log("LICENCE server is not listed on the Master: the pass names %s", server.c_str());
  }
  const auto challenge_answer = http::request(
      "POST", master_url + "/v1/licence/challenge",
      "{\"server\":" + json_string(server) + ",\"binding\":" + json_string(binding) + "}",
      kHttpTimeoutMs, 4096);
  const std::string challenge = text_field(challenge_answer.body, "challenge");
  if (challenge_answer.status != 200 || !challenge_answer.error.empty() || challenge.size() != 22 ||
      !base64url_text(challenge, 22)) {
    log("LICENCE Master challenge unavailable");
    return {};
  }

  std::string store;
  std::string ticket;
  std::string account;
  if (steam_module()) {
    store = "steam";
    account = steam_id();
    ticket = ownership_ticket(log);
  } else if (GetModuleHandleW(L"xgameruntime.dll")) {
    store = "xbox";
    ticket = xbox_licence_token(challenge, log);
  } else if (install_epic_licence_hooks(log)) {
    store = "epic";
    ticket = epic_licence_token(log);
  } else {
    log("LICENCE no supported online platform proof; Galaxy proof is not verified yet");
    unverified(
        "This copy of the game can't be verified. KCDS runs only on an original copy from "
        "Steam, the Epic Games Store or the Microsoft Store; GOG is not supported yet.");
    return {};
  }
  if (ticket.empty() || ticket.size() > 48 * 1024) {
    log("LICENCE platform could not produce a current licence proof");
    const char* name = store == "steam"  ? "Steam"
                       : store == "epic" ? "Epic Games"
                                         : "Microsoft Store";
    unverified(std::string(name) + " could not prove that you own the game. Start it from " + name +
               " while you are signed in and online, then try again.");
    return {};
  }
  // The hashed hardware id goes with the proof for the central ban list.
  const std::string device = hardware_id();
  const std::string fields =
      "{\"challenge\":" + json_string(challenge) + ",\"store\":" + json_string(store) +
      ",\"ticket\":" + json_string(ticket) + ",\"account\":" + json_string(account);
  std::string body = fields;
  if (!device.empty()) {
    body += ",\"device\":" + json_string(device);
  }
  // Who the player's Discord app is signed in as, so the Master can put their verified Discord
  // account into the pass or ask to link it.
  const std::string discord_user = discord::user_id();
  if (!discord_user.empty()) {
    body += ",\"discordUser\":" + json_string(discord_user);
  }
  // A server with the Kingdom Come: Deliverance map: the Master checks the player owns that game
  // on Steam and the pass says so.
  if (kcd1) {
    body += ",\"apps\":[379430]";
  }
  body += "}";
  auto answer = http::request("POST", master_url + "/v1/licence/proof", body, kHttpTimeoutMs, 4096);
  // A Master from before the central ban list refuses the unknown field before it takes the
  // challenge: the same proof again without it.
  if (answer.status == 400 && (!device.empty() || !discord_user.empty())) {
    log("LICENCE Master does not take the device yet; proof sent without it");
    answer =
        http::request("POST", master_url + "/v1/licence/proof", fields + "}", kHttpTimeoutMs, 4096);
  }
  const std::string pass = text_field(answer.body, "pass");
  if (answer.status == 403 && text_field(answer.body, "error") == "banned") {
    log("LICENCE Master refused: this player is banned from every KCDS server");
    if (refusal) {
      refusal->banned = true;
      refusal->text = ban_text(answer.body);
    }
    return {};
  }
  if (answer.status == 403 && kcd1) {
    const std::string error = text_field(answer.body, "error");
    const char* why = nullptr;
    if (error == "kcd1NotOwned") {
      why =
          "This server plays on the Kingdom Come: Deliverance map, and that game is not in your "
          "Steam library.";
    } else if (error == "kcd1Private") {
      why =
          "This server plays on the Kingdom Come: Deliverance map. To check you own that game, "
          "set Game details to Public in your Steam profile's privacy settings and join again.";
    } else if (error == "kcd1NeedsSteam") {
      why =
          "This server plays on the Kingdom Come: Deliverance map, which needs that game on "
          "Steam; it cannot be checked for this store.";
    } else if (error == "kcd1CheckUnavailable") {
      why =
          "This server plays on the Kingdom Come: Deliverance map, and the KCDS Master cannot "
          "check that game right now. Try again later.";
    }
    if (why) {
      log("LICENCE Master refused the Kingdom Come: Deliverance check: %s", error.c_str());
      if (refusal) {
        refusal->kcd1 = true;
      }
      unverified(why);
      return {};
    }
  }
  if (answer.status == 403 && text_field(answer.body, "error") == "notOwned") {
    log("LICENCE Master refused: the store says this account does not own the game");
    unverified(
        "The store says this account does not own Kingdom Come: Deliverance II. KCDS runs "
        "only on an original copy.");
    return {};
  }
  if (answer.status != 200 || !answer.error.empty() || !base64url_text(pass, 1024, true)) {
    log("LICENCE Master refused the platform proof (HTTP %d)", answer.status);
    return {};
  }
  log("LICENCE fresh %s pass bound to this server connection", store.c_str());
  const std::string expiry_key = "\"expires\":";
  const auto expiry_offset = answer.body.find(expiry_key);
  std::int64_t expiry = 0;
  if (expiry_offset == std::string::npos) {
    return {};
  }
  const auto parsed = std::from_chars(answer.body.data() + expiry_offset + expiry_key.size(),
                                      answer.body.data() + answer.body.size(), expiry);
  if (parsed.ec != std::errc{} || expiry <= 0) {
    return {};
  }
  if (expires) {
    *expires = expiry;
  }
  const std::string discord_link = text_field(answer.body, "discordLink");
  // Kept for a Verify Discord the player may choose (a server asking for a verified Discord
  // account, discord_login.hpp); nothing is asked of them here.
  if (!discord_link.empty() && base64url_text(discord_link, 22)) {
    remember_discord_ticket(master_url, discord_link);
    log("LICENCE the Master offers a Discord link for this account");
  } else if (!discord_link.empty()) {
    log("LICENCE the Master's Discord link ticket is not one (%zu bytes)", discord_link.size());
  }
  return pass;
}

}  // namespace kcds::runtime::identity
