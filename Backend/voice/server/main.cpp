// kcds-voice: the standalone voice server for large hosts (KCDS-DOCS/voice.md).
//
// Settings come from a JSON file (kcds-voice.json, or --config <file>), and the environment
// overrides them: KCDS_VOICE_BIND, KCDS_VOICE_PORT, KCDS_VOICE_CONTROL_PORT and KCDS_VOICE_TENANTS
// ("id=secret,id=secret").
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "kcd2s/sdk/nui.hpp"
#include "kcd2s/sdk/value.hpp"
#include "kcd2s/voice/voice_daemon.hpp"

namespace {

std::atomic<bool> stop_requested{false};

void on_stop_signal(int) {
  stop_requested = true;
}

std::string environment(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

bool parse_port(const std::string& text, std::uint16_t& port) {
  if (text.empty() || text.size() > 5 ||
      text.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  const unsigned long value = std::stoul(text);
  if (value == 0 || value > 65535) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

void read_file(const std::string& path, bool required, kcd2s::voice::VoiceDaemonOptions& options,
               bool& good) {
  using kcd2s::sdk::Tag;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (required) {
      std::cerr << "The settings file " << path << " could not be read" << std::endl;
      good = false;
    }
    return;
  }
  std::ostringstream text;
  text << file.rdbuf();
  const kcd2s::sdk::JsonResult parsed = kcd2s::sdk::json_to_value(text.str());
  if (!parsed.ok || parsed.value.tag() != Tag::Map) {
    std::cerr << path << " is not a JSON object" << std::endl;
    good = false;
    return;
  }
  for (const auto& [key, value] : parsed.value.as_map()) {
    if (key == "bind" && value.tag() == Tag::Str) {
      options.bind = value.as_str();
    } else if (key == "voicePort" && value.tag() == Tag::Int) {
      good = parse_port(std::to_string(value.as_int()), options.voice_port) && good;
    } else if (key == "controlPort" && value.tag() == Tag::Int) {
      good = parse_port(std::to_string(value.as_int()), options.control_port) && good;
    } else if (key == "tenants" && value.tag() == Tag::Map) {
      for (const auto& [id, secret] : value.as_map()) {
        if (secret.tag() == Tag::Str) {
          options.tenants[id] = secret.as_str();
        } else {
          std::cerr << path << ": the secret of a tenant is not text" << std::endl;
          good = false;
        }
      }
    } else {
      std::cerr << path << ": ignoring " << key << std::endl;
    }
  }
}

void read_tenants_variable(const std::string& text, kcd2s::voice::VoiceDaemonOptions& options,
                           bool& good) {
  std::stringstream entries(text);
  std::string entry;
  while (std::getline(entries, entry, ',')) {
    const std::size_t equals = entry.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 >= entry.size()) {
      std::cerr << "KCDS_VOICE_TENANTS: each entry is id=secret" << std::endl;
      good = false;
      continue;
    }
    options.tenants[entry.substr(0, equals)] = entry.substr(equals + 1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  kcd2s::voice::VoiceDaemonOptions options;
  options.log = [](const std::string& text) { std::cout << text << std::endl; };

  std::string config = "kcds-voice.json";
  bool config_required = false;
  for (int i = 1; i < argc; i += 2) {
    if (std::string(argv[i]) == "--config" && i + 1 < argc) {
      config = argv[i + 1];
      config_required = true;
    } else {
      std::cerr << "Use: kcds-voice [--config <file>]" << std::endl;
      return 2;
    }
  }

  bool good = true;
  read_file(config, config_required, options, good);
  if (!environment("KCDS_VOICE_BIND").empty()) {
    options.bind = environment("KCDS_VOICE_BIND");
  }
  if (!environment("KCDS_VOICE_PORT").empty()) {
    good = parse_port(environment("KCDS_VOICE_PORT"), options.voice_port) && good;
  }
  if (!environment("KCDS_VOICE_CONTROL_PORT").empty()) {
    good = parse_port(environment("KCDS_VOICE_CONTROL_PORT"), options.control_port) && good;
  }
  if (!environment("KCDS_VOICE_TENANTS").empty()) {
    read_tenants_variable(environment("KCDS_VOICE_TENANTS"), options, good);
  }
  if (!good) {
    return 2;
  }
  if (options.tenants.empty()) {
    std::cerr << "No game servers are allowed: set tenants in " << config
              << " or KCDS_VOICE_TENANTS" << std::endl;
    return 2;
  }

  std::signal(SIGINT, on_stop_signal);
  std::signal(SIGTERM, on_stop_signal);

  kcd2s::voice::VoiceDaemon daemon(options);
  if (!daemon.start()) {
    return 1;
  }
  while (!stop_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  daemon.stop();
  return 0;
}
