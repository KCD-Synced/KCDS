// Every control message tag in Backend/integration/include is its own: two features that picked
// the same value would decode each other's messages. The headers are read from the source tree, so
// a new header is checked without being listed here.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {

struct TagDeclaration {
  std::string name;
  std::string file;
};

// The tag declarations of every header: `inline constexpr std::uint8_t kSomethingTag = 42;`.
std::map<long, std::vector<TagDeclaration>> declared_tags() {
  const std::regex declaration(
      R"(constexpr\s+std::uint8_t\s+(k[A-Za-z0-9_]*Tag)\s*=\s*(0[xX][0-9A-Fa-f]+|[0-9]+)\s*;)");
  std::map<long, std::vector<TagDeclaration>> tags;
  const std::filesystem::path root(KCD2S_INTEGRATION_INCLUDE_DIR);
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".hpp") {
      continue;
    }
    std::ifstream in(entry.path());
    std::stringstream text;
    text << in.rdbuf();
    const std::string source = text.str();
    for (std::sregex_iterator it(source.begin(), source.end(), declaration), end; it != end; ++it) {
      const long value = std::stol((*it)[2].str(), nullptr, 0);
      tags[value].push_back({(*it)[1].str(), entry.path().filename().string()});
    }
  }
  return tags;
}

}  // namespace

KCD2S_TEST(wire_tags, finds_the_tags) {
  const auto tags = declared_tags();
  // The headers are there and the pattern still matches them: a few well-known tags.
  KCD2S_CHECK(tags.size() > 50);
  KCD2S_CHECK(tags.count(69) == 1);
  KCD2S_CHECK(tags.count(80) == 1);
}

KCD2S_TEST(wire_tags, every_value_is_unique) {
  for (const auto& [value, declarations] : declared_tags()) {
    if (declarations.size() < 2) {
      continue;
    }
    std::string names;
    for (const auto& declaration : declarations) {
      names += " " + declaration.name + " (" + declaration.file + ")";
    }
    std::fprintf(stderr, "    tag %ld is declared more than once:%s\n", value, names.c_str());
    KCD2S_CHECK(declarations.size() == 1);
  }
}
