#include "bug_report.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "kcd2s/core/crypto/sha256.hpp"
#include "kcd2s/testkit.hpp"
#include "kcds/launcher/json.hpp"

using namespace kcds::runtime::bug_report;

namespace {

namespace json = kcds::launcher::json;

// The body as JSON; a null value when it does not parse.
json::Value parsed(const std::string& text) {
  json::Limits limits;
  limits.max_bytes = 8 * 1024 * 1024;
  limits.max_depth = 4;
  json::Value value;
  std::string error;
  if (!json::parse(text, limits, value, error)) {
    return json::Value();
  }
  return value;
}

std::string hex(const std::uint8_t* bytes, std::size_t size) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < size; ++i) {
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 0x0F];
  }
  return out;
}

}  // namespace

// The report's body: the bot reads the text, the player's ids (for blocks) and the logs' ends.
KCD2S_TEST(bug_report, body_names_the_text_ids_and_logs) {
  const std::filesystem::path log =
      std::filesystem::temp_directory_path() / "kcds-bug-report-test.log";
  {
    std::ofstream file(log, std::ios::binary);
    file << "first line\nlast \"line\"\n";
  }
  Report report;
  report.text = "Sword \"floats\"\nafter a drop";
  report.nickname = "Hans";
  report.version = "0.3.4.2";
  report.server = "Test (127.0.0.1:7777)";
  report.steam_id = "76561198000000000";
  report.epic_id = "0123456789abcdef0123456789abcdef";
  report.discord_id = "123456789012345678";
  report.hardware_id = std::string(64, 'a');
  report.logs.emplace_back("client.log", log.wstring());
  report.logs.emplace_back("missing.log", (log.wstring() + L".none"));

  const json::Value body = parsed(body_of(report));
  KCD2S_CHECK(body.is_object());
  KCD2S_CHECK(body.member("accept").boolean());
  KCD2S_CHECK(body.member("text").string() == "Sword \"floats\"\nafter a drop");
  KCD2S_CHECK(body.member("nickname").string() == "Hans");
  const json::Value& ids = body.member("ids");
  KCD2S_CHECK(ids.member("steam").string() == "76561198000000000");
  KCD2S_CHECK(ids.member("epic").string() == "0123456789abcdef0123456789abcdef");
  KCD2S_CHECK(ids.member("discord").string() == "123456789012345678");
  KCD2S_CHECK(ids.member("hwid").string() == std::string(64, 'a'));
  // A log that is not there is left out.
  const json::Value& files = body.member("files");
  KCD2S_CHECK(files.is_array());
  KCD2S_CHECK(files.size() == 1);
  KCD2S_CHECK(files.at(0).member("name").string() == "client.log");
  KCD2S_CHECK(files.at(0).member("text").string() == "first line\nlast \"line\"\n");
  std::filesystem::remove(log);
}

KCD2S_TEST(bug_report, text_is_capped) {
  Report report;
  report.text = std::string(kMaxTextBytes + 500, 'x');
  const json::Value body = parsed(body_of(report));
  KCD2S_CHECK(body.is_object());
  KCD2S_CHECK(body.member("text").string().size() == kMaxTextBytes);
}

// The signature the bot checks: HMAC-SHA256 of "<t>.<body>" under the build's bug report key.
KCD2S_TEST(bug_report, signature_covers_time_and_body) {
  const std::string body = "{\"accept\":true}";
  const std::string query = signed_query(body, 1761700000);
  KCD2S_CHECK(query.rfind("?t=1761700000&s=", 0) == 0);
  const std::string signature = query.substr(query.find("&s=") + 3);
  KCD2S_CHECK(signature.size() == 64);

  constexpr std::array<std::uint8_t, 32> key = {KCDS_BUG_REPORT_KEY};
  const std::string message = "1761700000." + body;
  const auto digest = kcd2s::core::crypto::hmac_sha256(
      key.data(), key.size(), reinterpret_cast<const std::uint8_t*>(message.data()),
      message.size());
  KCD2S_CHECK(signature == hex(digest.data(), digest.size()));
  // Another time or another body is another signature.
  KCD2S_CHECK(signed_query(body, 1761700001).substr(16) != signature);
  KCD2S_CHECK(signed_query(body + " ", 1761700000).substr(16) != signature);
}

// The dialog's result message (shown for now): sent, or not sent and why.
KCD2S_TEST(bug_report, outcome_message_for_the_page) {
  Outcome sent;
  sent.status = 200;
  const json::Value ok = parsed(outcome_message(sent));
  KCD2S_CHECK(ok.member("type").string() == "bugReportResult");
  KCD2S_CHECK(ok.member("ok").boolean());

  Outcome refused;
  refused.status = 401;
  refused.error = "only the KCDS game sends \"bug\" reports";
  const json::Value no = parsed(outcome_message(refused));
  KCD2S_CHECK(no.is_object());
  KCD2S_CHECK(!no.member("ok").boolean());
  KCD2S_CHECK(no.member("status").number() == 401);
  KCD2S_CHECK(no.member("error").string() == "only the KCDS game sends \"bug\" reports");
}
