#pragma once
// The server's settings file and command line. Every setting has one name in
// server.json and one environment variable; a value is taken from, in this order:
//
//   1. the command line: --port, --bind, --level, --node, --resource-host, --resources, --config
//      (or the old positional form: port, bind address, level, node, resource host, resources)
//   2. the environment variable
//   3. server.json (next to where the server starts, or the --config file)
//   4. the documented default (KCDS-DOCS/server.md)
//
// server.json fills only the environment variables the environment leaves unset, so every reader
// of a variable (host_settings.hpp, master/registration_settings.hpp, the resource host) takes it
// as it always did, and a variable set by the owner wins. Unknown keys and values of the wrong type
// are reported and left out; nothing in the file can stop the server from starting.
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/sdk/nui.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcd2s::local::config {

// SwitchOrText: true or false (as "on" or "off"), or a text as it is ("on", "off" or an address).
enum class SettingKind { Text, Number, Switch, List, SwitchOrText };

struct FileSetting {
  const char* key;       // in server.json
  const char* variable;  // its environment variable
  SettingKind kind;
};

// The settings server.json can hold besides port, bind, level, node, resourceHost and resources.
inline constexpr FileSetting kFileSettings[] = {
    {"name", "KCDS_SERVER_NAME", SettingKind::Text},
    {"publicAddress", "KCDS_SERVER_PUBLIC_ADDRESS", SettingKind::Text},
    {"image", "KCDS_SERVER_IMAGE_URL", SettingKind::Text},
    {"description", "KCDS_SERVER_DESCRIPTION", SettingKind::Text},
    {"tags", "KCDS_SERVER_TAGS", SettingKind::List},
    {"website", "KCDS_SERVER_WEBSITE", SettingKind::Text},
    {"master", "KCDS_MASTER_URL", SettingKind::SwitchOrText},
    {"password", "KCDS_SERVER_PASSWORD", SettingKind::Text},
    {"requireDiscord", "KCDS_REQUIRE_DISCORD", SettingKind::Switch},
    {"kcd1Map", "KCDS_KCD1_MAP", SettingKind::Switch},
    {"maxPlayers", "KCDS_SERVER_MAX_PEERS", SettingKind::Number},
    {"tickRate", "KCDS_SERVER_TICK_HZ", SettingKind::Number},
    {"interestRadius", "KCDS_SERVER_INTEREST_RADIUS", SettingKind::Number},
    {"interestHysteresis", "KCDS_SERVER_INTEREST_HYSTERESIS", SettingKind::Number},
    {"maxCharacters", "KCDS_SERVER_MAX_CHARACTERS", SettingKind::Number},
    {"snapshotWorkers", "KCDS_SERVER_SNAPSHOT_WORKERS", SettingKind::Number},
    {"distanceRates", "KCDS_SERVER_DISTANCE_RATES", SettingKind::Switch},
    {"movementCheck", "KCDS_SERVER_MOVEMENT_CHECK", SettingKind::Text},
    {"meleeReachCheck", "KCDS_SERVER_MELEE_REACH_CHECK", SettingKind::Text},
    {"metrics", "KCDS_SERVER_METRICS", SettingKind::Number},
    {"luaCallLimit", "KCDS_LUA_CALL_LIMIT", SettingKind::Number},
    {"httpPort", "KCDS_HTTP_PORT", SettingKind::Number},
    {"httpBind", "KCDS_HTTP_BIND", SettingKind::Text},
    {"publicPlayerList", "KCDS_PUBLIC_PLAYER_LIST", SettingKind::Switch},
    {"revealMap", "KCDS_SERVER_REVEAL_MAP", SettingKind::Switch},
    {"levelPeople", "KCDS_SERVER_LEVEL_PEOPLE", SettingKind::Switch},
    {"npcHouses", "KCDS_SERVER_NPC_HOUSES", SettingKind::Switch},
    {"adminPort", "KCDS_ADMIN_PORT", SettingKind::Number},
    {"adminBind", "KCDS_ADMIN_BIND", SettingKind::Text},
    {"adminPassword", "KCDS_ADMIN_PASSWORD", SettingKind::Text},
    {"adminTlsCert", "KCDS_ADMIN_TLS_CERT", SettingKind::Text},
    {"adminTlsKey", "KCDS_ADMIN_TLS_KEY", SettingKind::Text},
    {"restartTimes", "KCDS_RESTART_TIMES", SettingKind::List},
    {"maps", "KCDS_SERVER_MAPS", SettingKind::List},
    {"voiceMode", "KCDS_SERVER_VOICE_MODE", SettingKind::Text},
    {"voiceUrl", "KCDS_SERVER_VOICE_URL", SettingKind::Text},
    {"voiceServerId", "KCDS_SERVER_VOICE_ID", SettingKind::Text},
    {"voiceSecret", "KCDS_SERVER_VOICE_SECRET", SettingKind::Text},
    {"voicePublicAddress", "KCDS_SERVER_VOICE_PUBLIC", SettingKind::Text},
};

// The start of the server: where it listens, its level and its resource host.
struct ServerStart {
  std::optional<std::string> port;
  std::optional<std::string> bind;
  std::optional<std::string> level;
  std::optional<std::string> node;
  std::optional<std::string> resource_host;
  std::optional<std::string> resources;
  std::optional<std::string> config;
};

// The command line, named (--port 7777 ...) or the old positional form. False with a problem for
// an unknown option or an option without its value.
inline bool parse_command_line(const std::vector<std::string>& arguments, ServerStart& start,
                               std::string& problem) {
  if (!arguments.empty() && arguments[0].rfind("--", 0) != 0) {
    std::optional<std::string>* positional[] = {&start.port,          &start.bind,
                                                &start.level,         &start.node,
                                                &start.resource_host, &start.resources};
    for (std::size_t i = 0; i < arguments.size() && i < 6; ++i) {
      *positional[i] = arguments[i];
    }
    return true;
  }
  const std::map<std::string, std::optional<std::string>*> named = {
      {"--port", &start.port},
      {"--bind", &start.bind},
      {"--level", &start.level},
      {"--node", &start.node},
      {"--resource-host", &start.resource_host},
      {"--resources", &start.resources},
      {"--config", &start.config},
  };
  for (std::size_t i = 0; i < arguments.size(); i += 2) {
    const auto found = named.find(arguments[i]);
    if (found == named.end()) {
      problem = "Unknown option " + arguments[i];
      return false;
    }
    if (i + 1 >= arguments.size()) {
      problem = arguments[i] + " needs a value";
      return false;
    }
    *found->second = arguments[i + 1];
  }
  return true;
}

// A setting's value as its variable takes it, or nullopt for a value of the wrong type.
inline std::optional<std::string> setting_text(const sdk::Value& value, SettingKind kind) {
  switch (kind) {
    case SettingKind::Text:
      if (value.tag() == sdk::Tag::Str) {
        return value.as_str();
      }
      return std::nullopt;
    case SettingKind::Number:
      if (value.tag() == sdk::Tag::Int) {
        return std::to_string(value.as_int());
      }
      return std::nullopt;
    case SettingKind::Switch:
      if (value.tag() == sdk::Tag::Bool) {
        return std::string(value.as_bool() ? "on" : "off");
      }
      return std::nullopt;
    case SettingKind::SwitchOrText:
      if (value.tag() == sdk::Tag::Bool) {
        return std::string(value.as_bool() ? "on" : "off");
      }
      if (value.tag() == sdk::Tag::Str) {
        return value.as_str();
      }
      return std::nullopt;
    case SettingKind::List: {
      if (value.tag() == sdk::Tag::Str) {
        return value.as_str();
      }
      if (value.tag() != sdk::Tag::Array) {
        return std::nullopt;
      }
      std::string joined;
      for (const sdk::Value& entry : value.as_array()) {
        if (entry.tag() != sdk::Tag::Str) {
          return std::nullopt;
        }
        joined += (joined.empty() ? "" : ",") + entry.as_str();
      }
      return joined;
    }
  }
  return std::nullopt;
}

struct ServerFile {
  // Variables to set: those of server.json the environment leaves unset.
  std::vector<std::pair<std::string, std::string>> variables;
  // port, bind, level, node, resourceHost and resources from the file.
  ServerStart start;
};

// Reads server.json's text. is_set(variable) tells whether the environment has the variable.
inline ServerFile read_server_file(const std::string& text,
                                   const std::function<bool(const char*)>& is_set,
                                   std::vector<std::string>& problems) {
  ServerFile file;
  const sdk::JsonResult parsed = sdk::json_to_value(text);
  if (!parsed.ok || parsed.value.tag() != sdk::Tag::Map) {
    problems.push_back("server.json is not a JSON object; it is not used");
    return file;
  }
  const std::map<std::string, std::optional<std::string>*> start_keys = {
      {"port", &file.start.port},
      {"bind", &file.start.bind},
      {"level", &file.start.level},
      {"node", &file.start.node},
      {"resourceHost", &file.start.resource_host},
      {"resources", &file.start.resources},
  };
  for (const auto& [key, value] : parsed.value.as_map()) {
    const auto start_key = start_keys.find(key);
    if (start_key != start_keys.end()) {
      const auto text_value =
          setting_text(value, key == "port" ? SettingKind::Number : SettingKind::Text);
      if (text_value) {
        *start_key->second = *text_value;
      } else {
        problems.push_back("server.json: ignoring " + key + " (wrong type)");
      }
      continue;
    }
    const FileSetting* setting = nullptr;
    for (const FileSetting& known : kFileSettings) {
      if (key == known.key) {
        setting = &known;
      }
    }
    if (!setting) {
      problems.push_back("server.json: ignoring unknown key " + key);
      continue;
    }
    const auto text_value = setting_text(value, setting->kind);
    if (!text_value) {
      problems.push_back("server.json: ignoring " + key + " (wrong type)");
      continue;
    }
    if (!is_set(setting->variable)) {
      file.variables.emplace_back(setting->variable, *text_value);
    }
  }
  return file;
}

// The first of the command line's, the environment's and the file's value, or the default.
inline std::string first_of(const std::optional<std::string>& command_line,
                            const std::optional<std::string>& environment,
                            const std::optional<std::string>& file, const std::string& fallback) {
  if (command_line) {
    return *command_line;
  }
  if (environment && !environment->empty()) {
    return *environment;
  }
  if (file) {
    return *file;
  }
  return fallback;
}

}  // namespace kcd2s::local::config
