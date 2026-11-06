// Portable part of the launcher-to-game crash settings hand-off.
#include "kcds/launcher/crash_handoff.hpp"

namespace kcds::launcher::crash {

std::vector<HandoffVariable> handoff_variables(const Settings& settings, bool allow_http_loopback) {
  bool valid = false;
  if (settings.enabled && is_tag(settings.environment)) {
    Dsn parsed;
    valid = parse_dsn(settings.dsn, allow_http_loopback, parsed);
  }

  std::vector<HandoffVariable> variables;
  if (!valid) {
    variables.push_back({kDsnVariable, ""});
    variables.push_back({kEnvironmentVariable, ""});
    variables.push_back({kAllowHttpLoopbackVariable, ""});
    return variables;
  }
  variables.push_back({kDsnVariable, settings.dsn});
  variables.push_back({kEnvironmentVariable, settings.environment});
  if (allow_http_loopback) {
    variables.push_back({kAllowHttpLoopbackVariable, "1"});
  } else {
    variables.push_back({kAllowHttpLoopbackVariable, ""});
  }
  return variables;
}

Handoff read_handoff(const std::string& dsn, const std::string& environment,
                     const std::string& allow_http_loopback) {
  Handoff handoff;
  handoff.settings.enabled = !dsn.empty();
  handoff.settings.dsn = dsn;
  if (!environment.empty()) {
    handoff.settings.environment = environment;
  }
  handoff.allow_http_loopback = allow_http_loopback == "1";
  return handoff;
}

}  // namespace kcds::launcher::crash
