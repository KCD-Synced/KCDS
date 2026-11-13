// Master-server registration settings of the local server.
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../local/master/registration_settings.hpp"
#include "../local/master/unlisted_count.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::local::master::read_registration_settings;
using kcd2s::local::master::RegistrationSettings;

namespace {

RegistrationSettings read_exactly(const std::map<std::string, std::string>& environment,
                                  const std::string& bind_ip, std::vector<std::string>& problems) {
  auto lookup = [&environment](const char* name) -> std::optional<std::string> {
    const auto found = environment.find(name);
    if (found == environment.end())
      return std::nullopt;
    return found->second;
  };
  return read_registration_settings(lookup, bind_ip, 7777, problems);
}

// The settings of a server whose owner turned the listing on, unless the case sets the master.
RegistrationSettings read(std::map<std::string, std::string> environment,
                          const std::string& bind_ip, std::vector<std::string>& problems) {
  environment.emplace("KCDS_MASTER_URL", "on");
  return read_exactly(environment, bind_ip, problems);
}

}  // namespace

KCD2S_TEST(master_registration, a_server_is_not_listed_by_default) {
  std::vector<std::string> problems;
  const auto settings = read_exactly({}, "0.0.0.0", problems);
  KCD2S_CHECK(!settings.enabled);
  KCD2S_CHECK(settings.reason.find("\"master\": \"on\"") != std::string::npos);
  KCD2S_CHECK(problems.empty());
}

KCD2S_TEST(master_registration, on_lists_the_server_on_the_kcds_master) {
  std::vector<std::string> problems;
  const auto settings = read_exactly({{"KCDS_MASTER_URL", "on"}}, "0.0.0.0", problems);
  KCD2S_CHECK(settings.enabled);
  KCD2S_CHECK_EQ(settings.master_url, std::string("https://master.kcdsynced.com"));
  KCD2S_CHECK_EQ(settings.address, std::string(":7777"));
  KCD2S_CHECK_EQ(settings.id, std::string("kcds-7777"));
  KCD2S_CHECK_EQ(settings.name, std::string("KCDS server"));
  KCD2S_CHECK(problems.empty());
}

KCD2S_TEST(master_registration, off_keeps_server_private) {
  std::vector<std::string> problems;
  const auto settings = read({{"KCDS_MASTER_URL", "off"}}, "0.0.0.0", problems);
  KCD2S_CHECK(!settings.enabled);
  KCD2S_CHECK(problems.empty());
}

KCD2S_TEST(master_registration, loopback_server_is_private_without_public_address) {
  std::vector<std::string> problems;
  const auto settings = read({}, "127.0.0.1", problems);
  KCD2S_CHECK(!settings.enabled);
  KCD2S_CHECK(settings.reason.find("127.0.0.1") != std::string::npos);

  const auto listed = read({{"KCDS_SERVER_PUBLIC_ADDRESS", "127.0.0.1:7796"},
                            {"KCDS_SERVER_NAME", "KCDS live test"},
                            {"KCDS_MASTER_URL", "http://127.0.0.1:8080/"}},
                           "127.0.0.1", problems);
  KCD2S_CHECK(listed.enabled);
  KCD2S_CHECK_EQ(listed.address, std::string("127.0.0.1:7796"));
  KCD2S_CHECK_EQ(listed.name, std::string("KCDS live test"));
  KCD2S_CHECK_EQ(listed.master_url, std::string("http://127.0.0.1:8080"));
  KCD2S_CHECK(problems.empty());
}

KCD2S_TEST(master_registration, invalid_values_are_reported) {
  std::vector<std::string> problems;
  KCD2S_CHECK(!read({{"KCDS_MASTER_URL", "http://master.example"}}, "0.0.0.0", problems).enabled);
  KCD2S_CHECK(!read({{"KCDS_MASTER_URL", "ftp://x"}}, "0.0.0.0", problems).enabled);
  KCD2S_CHECK(
      !read({{"KCDS_MASTER_URL", "https://user@master.example"}}, "0.0.0.0", problems).enabled);
  KCD2S_CHECK(!read({{"KCDS_SERVER_PUBLIC_ADDRESS", "nope"}}, "0.0.0.0", problems).enabled);
  KCD2S_CHECK_EQ(problems.size(), std::size_t{4});

  problems.clear();
  // A password server is listed with a lock; the password itself stays here.
  KCD2S_CHECK(read({{"KCDS_SERVER_PASSWORD", "secret"}}, "0.0.0.0", problems).password);
  KCD2S_CHECK(!read({{"KCDS_SERVER_PASSWORD", ""}}, "0.0.0.0", problems).password);
  KCD2S_CHECK(!read({}, "0.0.0.0", problems).password);
  const auto pictured =
      read({{"KCDS_SERVER_IMAGE_URL", "https://cdn.example.com/art.jpg"}}, "0.0.0.0", problems);
  KCD2S_CHECK_EQ(pictured.image, std::string("https://cdn.example.com/art.jpg"));
  KCD2S_CHECK(problems.empty());
  for (const char* bad : {"http://cdn.example.com/a.jpg", "https://cdn.example.com/a.jpg')",
                          "https://user@cdn.example.com/a.jpg", "https:///a.jpg"}) {
    const auto rejected = read({{"KCDS_SERVER_IMAGE_URL", bad}}, "0.0.0.0", problems);
    KCD2S_CHECK(rejected.enabled);
    KCD2S_CHECK(rejected.image.empty());
  }
  KCD2S_CHECK_EQ(problems.size(), std::size_t{4});

  problems.clear();
  const auto unnamed = read({{"KCDS_SERVER_NAME", std::string(65, 'x')}}, "0.0.0.0", problems);
  KCD2S_CHECK(unnamed.enabled);
  KCD2S_CHECK_EQ(unnamed.name, std::string("KCDS server"));
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});
}

KCD2S_TEST(master_registration, description_tags_and_website_for_the_browser) {
  std::vector<std::string> problems;
  const auto settings = read({{"KCDS_SERVER_DESCRIPTION", "  Medieval roleplay around Trosky.  "},
                              {"KCDS_SERVER_TAGS", "roleplay, Voice chat,My own,Roleplay,18+"},
                              {"KCDS_SERVER_WEBSITE", "https://crownrp.cz"}},
                             "0.0.0.0", problems);
  KCD2S_CHECK_EQ(settings.description, std::string("Medieval roleplay around Trosky."));
  KCD2S_CHECK_EQ(settings.website, std::string("https://crownrp.cz"));
  // Known tags in the list's spelling, once each; the unknown one is reported.
  KCD2S_CHECK_EQ(settings.tags.size(), std::size_t{3});
  KCD2S_CHECK_EQ(settings.tags[0], std::string("Roleplay"));
  KCD2S_CHECK_EQ(settings.tags[1], std::string("Voice chat"));
  KCD2S_CHECK_EQ(settings.tags[2], std::string("18+"));
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});

  problems.clear();
  const auto plain = read({{"KCDS_SERVER_WEBSITE", "http://example.com/rp"}}, "0.0.0.0", problems);
  KCD2S_CHECK_EQ(plain.website, std::string("http://example.com/rp"));
  KCD2S_CHECK(problems.empty());
  for (const char* bad : {"ftp://example.com", "javascript:alert(1)", "https://a@example.com",
                          "https://example.com/\"x"}) {
    const auto rejected = read({{"KCDS_SERVER_WEBSITE", bad}}, "0.0.0.0", problems);
    KCD2S_CHECK(rejected.website.empty());
  }
  KCD2S_CHECK_EQ(problems.size(), std::size_t{4});

  problems.clear();
  const auto long_text =
      read({{"KCDS_SERVER_DESCRIPTION", std::string(601, 'x')}}, "0.0.0.0", problems);
  KCD2S_CHECK(long_text.description.empty());
  KCD2S_CHECK_EQ(problems.size(), std::size_t{1});
}

// An unlisted server reports only its player count, and only when others can reach it.
KCD2S_TEST(master_registration, an_unlisted_server_reports_its_player_count) {
  using kcd2s::local::master::reports_unlisted_count;
  using kcd2s::local::master::unlisted_count_body;
  std::vector<std::string> problems;
  const auto unlisted = read_exactly({}, "0.0.0.0", problems);
  KCD2S_CHECK(reports_unlisted_count(unlisted, "0.0.0.0"));
  KCD2S_CHECK(!reports_unlisted_count(unlisted, "127.0.0.1"));
  const auto listed = read({}, "0.0.0.0", problems);
  KCD2S_CHECK(!reports_unlisted_count(listed, "0.0.0.0"));

  const std::string id = kcd2s::local::master::unlisted_run_id();
  KCD2S_CHECK_EQ(id.size(), std::size_t{16});
  KCD2S_CHECK_EQ(unlisted_count_body("0123456789abcdef", 7, "1.0.7"),
                 std::string("{\"id\":\"0123456789abcdef\",\"players\":7,\"version\":\"1.0.7\"}"));
  KCD2S_CHECK_EQ(unlisted_count_body("0123456789abcdef", -1, "1.0.7\"x"),
                 std::string("{\"id\":\"0123456789abcdef\",\"players\":0,\"version\":\"1.0.7\"}"));
}
