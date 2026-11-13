#include "package_manifest.hpp"

#include <cctype>
#include <cstddef>

namespace kcds::runtime::launch {
namespace {

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The XML entities an attribute value may hold; any other '&' text is kept as it is.
std::string decode_entities(std::string_view text) {
  struct Entity {
    std::string_view name;
    char value;
  };
  constexpr Entity entities[] = {
      {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}};
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    bool replaced = false;
    if (text[i] == '&') {
      for (const Entity& entity : entities) {
        if (text.substr(i, entity.name.size()) == entity.name) {
          out.push_back(entity.value);
          i += entity.name.size();
          replaced = true;
          break;
        }
      }
    }
    if (!replaced) {
      out.push_back(text[i]);
      ++i;
    }
  }
  return out;
}

// The text of the first start tag named element (followed by a space, so "<Application" does
// not match "<Applications>"), without its "<" and ">"; empty when there is none.
std::string_view find_start_tag(std::string_view text, std::string_view element) {
  const std::string open = "<" + std::string(element);
  for (std::size_t at = text.find(open); at != std::string_view::npos;
       at = text.find(open, at + 1)) {
    const std::size_t after = at + open.size();
    if (after < text.size() && is_space(text[after])) {
      const std::size_t end = text.find('>', after);
      if (end == std::string_view::npos) {
        return {};
      }
      return text.substr(at + 1, end - at - 1);
    }
  }
  return {};
}

// The decoded value of attribute name in a start tag's text, or nullopt.
std::optional<std::string> attribute(std::string_view tag, std::string_view name) {
  for (std::size_t at = tag.find(name); at != std::string_view::npos; at = tag.find(name, at + 1)) {
    // A whole attribute name: after a space and followed by '=' (spaces allowed around it).
    if (at == 0 || !is_space(tag[at - 1])) {
      continue;
    }
    std::size_t i = at + name.size();
    while (i < tag.size() && is_space(tag[i])) {
      ++i;
    }
    if (i >= tag.size() || tag[i] != '=') {
      continue;
    }
    ++i;
    while (i < tag.size() && is_space(tag[i])) {
      ++i;
    }
    if (i >= tag.size() || (tag[i] != '"' && tag[i] != '\'')) {
      return std::nullopt;
    }
    const char quote = tag[i];
    const std::size_t end = tag.find(quote, i + 1);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    return decode_entities(tag.substr(i + 1, end - i - 1));
  }
  return std::nullopt;
}

}  // namespace

std::optional<PackageIdentity> parse_package_identity(std::string_view manifest) {
  const std::string_view identity = find_start_tag(manifest, "Identity");
  if (identity.empty()) {
    return std::nullopt;
  }
  PackageIdentity result;
  const auto name = attribute(identity, "Name");
  const auto publisher = attribute(identity, "Publisher");
  if (!name || !publisher || name->empty() || publisher->empty()) {
    return std::nullopt;
  }
  result.name = *name;
  result.publisher = *publisher;
  // appxmanifest.xml: <Application Id="...">; MicrosoftGame.config: <Executable Id="...">.
  std::string_view application = find_start_tag(manifest, "Application");
  if (application.empty()) {
    application = find_start_tag(manifest, "Executable");
  }
  if (!application.empty()) {
    if (const auto id = attribute(application, "Id")) {
      result.app_id = *id;
    }
  }
  return result;
}

}  // namespace kcds::runtime::launch
