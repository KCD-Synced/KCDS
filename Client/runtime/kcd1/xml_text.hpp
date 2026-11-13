// Small edits of the level's XML text as the editor writes it (one tag per line, attributes in
// double quotes), without parsing the whole file. Plain code, no game calls.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace kcds::runtime::kcd1::xml {

std::string lower(std::string_view text);
bool contains(std::string_view text, std::string_view part);

// The value of a tag's attribute; empty when it has none.
std::string attribute(std::string_view tag, std::string_view name);
bool has_attribute(std::string_view tag, std::string_view name);

// The tag with the attribute set (added before the tag's end when it has none).
std::string with_attribute(const std::string& tag, std::string_view name, std::string_view value);
std::string without_attribute(const std::string& tag, std::string_view name);

// Where the element's first <name ...> tag starts, npos when it has none.
std::size_t tag_at(const std::string& element, std::string_view name);

// The element with its first <name ...> tag changed by `edit`.
template <typename Edit>
std::string with_tag(const std::string& element, std::string_view name, Edit edit) {
  const std::size_t at = tag_at(element, name);
  if (at == std::string::npos) {
    return element;
  }
  const std::size_t end = element.find('>', at);
  if (end == std::string::npos) {
    return element;
  }
  const std::string tag = element.substr(at, end + 1 - at);
  return element.substr(0, at) + edit(tag) + element.substr(end + 1);
}

}  // namespace kcds::runtime::kcd1::xml
