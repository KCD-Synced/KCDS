// Crash-reporting plumbing: DSN validation follows the update-origin rule, and
// start() never touches a backend unless reporting is explicitly enabled with a
// valid DSN. The launcher-to-game hand-off exports only valid, enabled
// settings. Nothing here installs a handler or sends anything.
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"
#include "kcds/launcher/crash.hpp"
#include "kcds/launcher/crash_handoff.hpp"
#include "kcds/launcher/crash_test.hpp"

namespace {

using kcds::launcher::crash::Backend;
using kcds::launcher::crash::Dsn;
using kcds::launcher::crash::RuntimeInfo;
using kcds::launcher::crash::Settings;
using kcds::launcher::crash::State;

class RecordingBackend : public Backend {
 public:
  bool start(const Dsn& dsn, const Settings& settings, const RuntimeInfo& info) override {
    ++starts;
    last_dsn = dsn;
    last_environment = settings.environment;
    last_release = info.release;
    return accept;
  }

  void stop() override {
    ++stops;
  }

  bool accept = true;
  int starts = 0;
  int stops = 0;
  Dsn last_dsn;
  std::string last_environment;
  std::string last_release;
};

const char* kDsn = "https://0123456789abcdef0123@crash.kcds.invalid/42";

}  // namespace

KCD2S_TEST(crash, dsn_is_parsed_into_origin_key_and_project) {
  Dsn dsn;
  KCD2S_CHECK(kcds::launcher::crash::parse_dsn(kDsn, false, dsn));
  KCD2S_CHECK_EQ(dsn.origin, std::string("https://crash.kcds.invalid"));
  KCD2S_CHECK_EQ(dsn.public_key, std::string("0123456789abcdef0123"));
  KCD2S_CHECK_EQ(dsn.project_id, std::string("42"));

  KCD2S_CHECK(kcds::launcher::crash::parse_dsn("https://0123456789abcdef@Crash.Kcds.invalid:8443/7",
                                               false, dsn));
  KCD2S_CHECK_EQ(dsn.origin, std::string("https://crash.kcds.invalid:8443"));
}

KCD2S_TEST(crash, malformed_dsns_are_refused) {
  Dsn dsn;
  const char* invalid[] = {
      "",
      "crash.kcds.invalid/42",
      "http://0123456789abcdef@crash.kcds.invalid/42",
      "https://crash.kcds.invalid/42",
      "https://short@crash.kcds.invalid/42",
      "https://0123456789abcdef:secret@crash.kcds.invalid/42",
      "https://0123456789abcdef@crash.kcds.invalid/",
      "https://0123456789abcdef@crash.kcds.invalid/api/42",
      "https://0123456789abcdef@crash.kcds.invalid/42?x=1",
      "https://0123456789abcdef@crash.kcds.invalid/42#f",
      "https://0123456789abcdef@crash example.test/42",
      "ftp://0123456789abcdef@crash.kcds.invalid/42",
  };
  for (const char* value : invalid) {
    KCD2S_CHECK(!kcds::launcher::crash::parse_dsn(value, false, dsn));
  }
  // Loopback HTTP only with the development switch.
  KCD2S_CHECK(
      !kcds::launcher::crash::parse_dsn("http://0123456789abcdef@127.0.0.1:17800/1", false, dsn));
  KCD2S_CHECK(
      kcds::launcher::crash::parse_dsn("http://0123456789abcdef@127.0.0.1:17800/1", true, dsn));
}

KCD2S_TEST(crash, disabled_settings_never_touch_the_backend) {
  RecordingBackend backend;
  Settings settings;  // defaults: disabled, no DSN
  RuntimeInfo info;
  KCD2S_CHECK(kcds::launcher::crash::start(settings, false, info, &backend) == State::Disabled);
  settings.dsn = kDsn;  // a DSN alone does not enable reporting
  KCD2S_CHECK(kcds::launcher::crash::start(settings, false, info, &backend) == State::Disabled);
  KCD2S_CHECK_EQ(backend.starts, 0);
}

KCD2S_TEST(crash, invalid_enabled_settings_never_touch_the_backend) {
  RecordingBackend backend;
  RuntimeInfo info;
  Settings settings;
  settings.enabled = true;
  KCD2S_CHECK(kcds::launcher::crash::start(settings, false, info, &backend) ==
              State::InvalidSettings);
  settings.dsn = "http://0123456789abcdef@crash.kcds.invalid/42";
  KCD2S_CHECK(kcds::launcher::crash::start(settings, false, info, &backend) ==
              State::InvalidSettings);
  settings.dsn = kDsn;
  settings.environment = "";
  KCD2S_CHECK(kcds::launcher::crash::start(settings, false, info, &backend) ==
              State::InvalidSettings);
  KCD2S_CHECK_EQ(backend.starts, 0);
}

KCD2S_TEST(crash, enabled_without_backend_reports_unavailable) {
  Settings settings;
  settings.enabled = true;
  settings.dsn = kDsn;
  KCD2S_CHECK(kcds::launcher::crash::start(settings, false, RuntimeInfo{}, nullptr) ==
              State::BackendUnavailable);
}

KCD2S_TEST(crash, enabled_valid_settings_start_the_backend_with_exact_values) {
  RecordingBackend backend;
  Settings settings;
  settings.enabled = true;
  settings.dsn = kDsn;
  settings.environment = "staging";
  RuntimeInfo info;
  info.release = "kcds-launcher@0.1.0";
  KCD2S_CHECK(kcds::launcher::crash::start(settings, false, info, &backend) == State::Started);
  KCD2S_CHECK_EQ(backend.starts, 1);
  KCD2S_CHECK_EQ(backend.last_dsn.project_id, std::string("42"));
  KCD2S_CHECK_EQ(backend.last_environment, std::string("staging"));
  KCD2S_CHECK_EQ(backend.last_release, std::string("kcds-launcher@0.1.0"));

  backend.accept = false;
  KCD2S_CHECK(kcds::launcher::crash::start(settings, false, info, &backend) == State::StartFailed);
}

KCD2S_TEST(crash, tags_are_restricted) {
  KCD2S_CHECK(kcds::launcher::crash::is_tag("production"));
  KCD2S_CHECK(kcds::launcher::crash::is_tag("kcds-launcher_0.1.1"));
  KCD2S_CHECK(!kcds::launcher::crash::is_tag(""));
  KCD2S_CHECK(!kcds::launcher::crash::is_tag("Production"));
  KCD2S_CHECK(!kcds::launcher::crash::is_tag("a b"));
  KCD2S_CHECK(!kcds::launcher::crash::is_tag(std::string(65, 'a')));
}

namespace {

std::string handoff_value(const std::vector<kcds::launcher::crash::HandoffVariable>& variables,
                          const std::string& name) {
  for (const auto& variable : variables) {
    if (variable.name == name) {
      return variable.value;
    }
  }
  return "<missing>";
}

}  // namespace

KCD2S_TEST(crash, handoff_exports_enabled_valid_settings) {
  Settings settings;
  settings.enabled = true;
  settings.dsn = kDsn;
  settings.environment = "staging";
  auto variables = kcds::launcher::crash::handoff_variables(settings, false);
  KCD2S_CHECK_EQ(variables.size(), std::size_t(3));
  KCD2S_CHECK_EQ(handoff_value(variables, "KCDS_CRASH_DSN"), std::string(kDsn));
  KCD2S_CHECK_EQ(handoff_value(variables, "KCDS_CRASH_ENVIRONMENT"), std::string("staging"));
  KCD2S_CHECK_EQ(handoff_value(variables, "KCDS_CRASH_ALLOW_HTTP_LOOPBACK"), std::string(""));

  settings.dsn = "http://0123456789abcdef@127.0.0.1:17800/1";
  variables = kcds::launcher::crash::handoff_variables(settings, true);
  KCD2S_CHECK_EQ(handoff_value(variables, "KCDS_CRASH_DSN"), settings.dsn);
  KCD2S_CHECK_EQ(handoff_value(variables, "KCDS_CRASH_ALLOW_HTTP_LOOPBACK"), std::string("1"));
}

KCD2S_TEST(crash, handoff_clears_disabled_or_invalid_settings) {
  Settings settings;  // disabled, no DSN
  settings.dsn = kDsn;
  auto variables = kcds::launcher::crash::handoff_variables(settings, false);
  KCD2S_CHECK_EQ(variables.size(), std::size_t(3));
  for (const auto& variable : variables) {
    KCD2S_CHECK(variable.value.empty());
  }

  settings.enabled = true;
  settings.dsn = "http://0123456789abcdef@crash.kcds.invalid/42";  // plain HTTP, not loopback
  variables = kcds::launcher::crash::handoff_variables(settings, true);
  for (const auto& variable : variables) {
    KCD2S_CHECK(variable.value.empty());
  }
}

KCD2S_TEST(crash, handoff_is_read_back_by_the_game_client) {
  auto handoff = kcds::launcher::crash::read_handoff("", "", "");
  KCD2S_CHECK(!handoff.settings.enabled);
  KCD2S_CHECK(kcds::launcher::crash::start(handoff.settings, handoff.allow_http_loopback,
                                           RuntimeInfo{}, nullptr) == State::Disabled);

  handoff = kcds::launcher::crash::read_handoff(kDsn, "", "");
  KCD2S_CHECK(handoff.settings.enabled);
  KCD2S_CHECK_EQ(handoff.settings.environment, std::string("production"));
  KCD2S_CHECK(!handoff.allow_http_loopback);

  handoff = kcds::launcher::crash::read_handoff("http://0123456789abcdef@127.0.0.1:17800/1",
                                                "staging", "1");
  KCD2S_CHECK(handoff.allow_http_loopback);
  KCD2S_CHECK_EQ(handoff.settings.environment, std::string("staging"));

  // The game client validates again: a tampered value never starts a backend.
  RecordingBackend backend;
  handoff = kcds::launcher::crash::read_handoff("http://0123456789abcdef@example.test/1", "", "");
  KCD2S_CHECK(kcds::launcher::crash::start(handoff.settings, handoff.allow_http_loopback,
                                           RuntimeInfo{}, &backend) == State::InvalidSettings);
  KCD2S_CHECK_EQ(backend.starts, 0);
}

#ifdef _WIN32
KCD2S_TEST(crash, handoff_round_trips_through_the_process_environment) {
  Settings settings;
  settings.enabled = true;
  settings.dsn = "http://0123456789abcdef@127.0.0.1:17800/1";
  settings.environment = "staging";
  KCD2S_CHECK(kcds::launcher::crash::export_handoff(
      kcds::launcher::crash::handoff_variables(settings, true)));
  auto handoff = kcds::launcher::crash::read_handoff_from_environment();
  KCD2S_CHECK(handoff.settings.enabled);
  KCD2S_CHECK_EQ(handoff.settings.dsn, settings.dsn);
  KCD2S_CHECK_EQ(handoff.settings.environment, std::string("staging"));
  KCD2S_CHECK(handoff.allow_http_loopback);

  // Disabled settings remove every variable again.
  KCD2S_CHECK(kcds::launcher::crash::export_handoff(
      kcds::launcher::crash::handoff_variables(Settings{}, false)));
  handoff = kcds::launcher::crash::read_handoff_from_environment();
  KCD2S_CHECK(!handoff.settings.enabled);
  KCD2S_CHECK(handoff.settings.dsn.empty());
  KCD2S_CHECK(!handoff.allow_http_loopback);
}
#endif

KCD2S_TEST(crash, crash_test_switch_accepts_only_exact_targets) {
  using kcds::launcher::crash::CrashTest;
  using kcds::launcher::crash::parse_crash_test;
  KCD2S_CHECK(parse_crash_test("launcher") == CrashTest::Launcher);
  KCD2S_CHECK(parse_crash_test("client") == CrashTest::Client);
  KCD2S_CHECK(parse_crash_test("") == CrashTest::None);
  KCD2S_CHECK(parse_crash_test("Launcher") == CrashTest::None);
  KCD2S_CHECK(parse_crash_test("1") == CrashTest::None);
  KCD2S_CHECK(parse_crash_test("client ") == CrashTest::None);
}

KCD2S_TEST(crash, crash_test_delay_is_bounded) {
  using kcds::launcher::crash::kDefaultCrashTestDelaySeconds;
  using kcds::launcher::crash::parse_crash_test_delay;
  KCD2S_CHECK_EQ(parse_crash_test_delay(""), kDefaultCrashTestDelaySeconds);
  KCD2S_CHECK_EQ(parse_crash_test_delay("0"), 0u);
  KCD2S_CHECK_EQ(parse_crash_test_delay("45"), 45u);
  KCD2S_CHECK_EQ(parse_crash_test_delay("600"), 600u);
  KCD2S_CHECK_EQ(parse_crash_test_delay("601"), kDefaultCrashTestDelaySeconds);
  KCD2S_CHECK_EQ(parse_crash_test_delay("-5"), kDefaultCrashTestDelaySeconds);
  KCD2S_CHECK_EQ(parse_crash_test_delay("99999"), kDefaultCrashTestDelaySeconds);
}
