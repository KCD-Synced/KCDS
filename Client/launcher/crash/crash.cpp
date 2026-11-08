#include "kcds/launcher/crash.hpp"

#include "kcds/launcher/origin.hpp"

namespace kcds::launcher::crash {

namespace {

bool is_alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool is_public_key(const std::string& value) {
  if (value.size() < 16 || value.size() > 128) {
    return false;
  }
  for (char c : value) {
    if (!is_alnum(c)) {
      return false;
    }
  }
  return true;
}

bool is_project_id(const std::string& value) {
  if (value.empty() || value.size() > 20) {
    return false;
  }
  for (char c : value) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

}  // namespace

bool is_tag(const std::string& value) {
  static constexpr char kAllowed[] = "abcdefghijklmnopqrstuvwxyz0123456789_.-";
  return !value.empty() && value.size() <= 64 &&
         value.find_first_not_of(kAllowed) == std::string::npos;
}

bool parse_dsn(const std::string& dsn, bool allow_http_loopback, Dsn& out) {
  std::size_t scheme_end = dsn.find("://");
  if (scheme_end == std::string::npos) {
    return false;
  }
  std::string scheme = dsn.substr(0, scheme_end);
  std::string rest = dsn.substr(scheme_end + 3);

  // {public_key}@{authority}/{project_id}. A ':' inside the user part would be
  // a secret key, which a client DSN must never carry.
  std::size_t at = rest.find('@');
  if (at == std::string::npos) {
    return false;
  }
  std::string public_key = rest.substr(0, at);
  std::string location = rest.substr(at + 1);
  std::size_t slash = location.find('/');
  if (slash == std::string::npos) {
    return false;
  }
  std::string authority = location.substr(0, slash);
  std::string project_id = location.substr(slash + 1);
  if (!is_public_key(public_key) || !is_project_id(project_id)) {
    return false;
  }

  std::string origin;
  if (!feed::normalize_origin(scheme + "://" + authority, allow_http_loopback, origin)) {
    return false;
  }

  out.origin = origin;
  out.public_key = public_key;
  out.project_id = project_id;
  return true;
}

State start(const Settings& settings, bool allow_http_loopback, const RuntimeInfo& info,
            Backend* backend) {
  if (!settings.enabled) {
    return State::Disabled;
  }
  Dsn dsn;
  if (settings.dsn.empty() || !parse_dsn(settings.dsn, allow_http_loopback, dsn) ||
      !is_tag(settings.environment)) {
    return State::InvalidSettings;
  }
  if (backend == nullptr) {
    return State::BackendUnavailable;
  }
  if (!backend->start(dsn, settings, info)) {
    return State::StartFailed;
  }
  return State::Started;
}

const char* describe(State state) {
  switch (state) {
    case State::Disabled:
      return "crash reporting disabled by configuration";
    case State::InvalidSettings:
      return "crash reporting not started: invalid settings";
    case State::BackendUnavailable:
      return "crash reporting enabled, but this build has no native crash backend";
    case State::Started:
      return "crash reporting started";
    case State::StartFailed:
      return "crash reporting backend failed to start";
  }
  return "crash reporting state unknown";
}

const char* release_dsn() {
#ifdef KCDS_RELEASE_CRASH_DSN
  return KCDS_RELEASE_CRASH_DSN;
#else
  return "";
#endif
}

Settings release_default(const Settings& configured, const std::string& release_dsn) {
  if (release_dsn.empty() || (configured.enabled && !configured.dsn.empty())) {
    return configured;
  }
  Settings settings;
  settings.enabled = true;
  settings.dsn = release_dsn;
  settings.environment = "production";
  return settings;
}

}  // namespace kcds::launcher::crash
