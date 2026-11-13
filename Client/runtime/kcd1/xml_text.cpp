#include "xml_text.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>

namespace kcds::runtime::kcd1::xml {
namespace {

// Where the value of a tag's attribute starts (after its quote), npos when there is none.
std::size_t value_at(std::string_view tag, std::string_view name) {
  std::size_t at = 0;
  while ((at = tag.find(name, at)) != std::string_view::npos) {
    const bool starts = at > 0 && std::isspace(unsigned(tag[at - 1]));
    const std::size_t quote = at + name.size();
    if (starts && tag.substr(quote, 2) == "=\"") {
      return quote + 2;
    }
    at += name.size();
  }
  return std::string_view::npos;
}

}  // namespace

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = char(std::tolower(unsigned(c)));
  }
  return out;
}

bool contains(std::string_view text, std::string_view part) {
  return text.find(part) != std::string_view::npos;
}

std::string attribute(std::string_view tag, std::string_view name) {
  const std::size_t at = value_at(tag, name);
  if (at == std::string_view::npos) {
    return {};
  }
  const std::size_t end = tag.find('"', at);
  return end == std::string_view::npos ? std::string() : std::string(tag.substr(at, end - at));
}

bool has_attribute(std::string_view tag, std::string_view name) {
  return value_at(tag, name) != std::string_view::npos;
}

std::string with_attribute(const std::string& tag, std::string_view name, std::string_view value) {
  const std::size_t at = value_at(tag, name);
  if (at != std::string::npos) {
    const std::size_t end = tag.find('"', at);
    return tag.substr(0, at) + std::string(value) + tag.substr(end);
  }
  std::size_t end = tag.size() - 1;
  if (end > 0 && tag[end - 1] == '/') {
    end -= 1;
  }
  return tag.substr(0, end) + " " + std::string(name) + "=\"" + std::string(value) + "\"" +
         tag.substr(end);
}

std::string without_attribute(const std::string& tag, std::string_view name) {
  const std::size_t at = value_at(tag, name);
  if (at == std::string::npos) {
    return tag;
  }
  const std::size_t start = at - name.size() - 3;  // the space before the name
  const std::size_t end = tag.find('"', at) + 1;
  return tag.substr(0, start) + tag.substr(end);
}

std::size_t tag_at(const std::string& element, std::string_view name) {
  const std::string open = "<" + std::string(name);
  std::size_t at = 0;
  while ((at = element.find(open, at)) != std::string::npos) {
    const char next = at + open.size() < element.size() ? element[at + open.size()] : '\0';
    if (next == ' ' || next == '>' || next == '/') {
      return at;
    }
    at += open.size();
  }
  return std::string::npos;
}

void each_entity(
    const std::string& text,
    const std::function<void(const std::string& element, const std::string& head)>& fn) {
  std::size_t at = 0;
  while ((at = text.find("<Entity ", at)) != std::string::npos) {
    const std::size_t head_end = text.find('>', at);
    if (head_end == std::string::npos) {
      return;
    }
    std::size_t end = head_end + 1;
    if (text[head_end - 1] != '/') {
      const std::size_t close = text.find("</Entity>", head_end);
      if (close == std::string::npos) {
        return;
      }
      end = close + std::string_view("</Entity>").size();
    }
    fn(text.substr(at, end - at), text.substr(at, head_end + 1 - at));
    at = end;
  }
}

std::string element_of(const std::string& text, std::string_view name, std::size_t from) {
  const std::size_t at = tag_at(text.substr(from), name);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t start = from + at;
  const std::size_t head_end = text.find('>', start);
  if (head_end == std::string::npos) {
    return {};
  }
  if (text[head_end - 1] == '/') {
    return text.substr(start, head_end + 1 - start);
  }
  const std::string close = "</" + std::string(name) + ">";
  const std::size_t end = text.find(close, head_end);
  if (end == std::string::npos) {
    return {};
  }
  return text.substr(start, end + close.size() - start);
}

std::string guid_digits(const std::string& value) {
  std::string out;
  for (const char c : lower(value)) {
    if (c != '-') {
      out += c;
    }
  }
  return out;
}

std::string guid_written(const std::string& digits) {
  if (digits.size() != 16) {
    return digits;
  }
  return digits.substr(0, 8) + "-" + digits.substr(8, 4) + "-" + digits.substr(12);
}

std::string derived_guid(const std::string& seed) {
  std::uint64_t hash = 0xcbf29ce484222325ull;
  for (const char c : seed) {
    hash ^= std::uint8_t(c);
    hash *= 0x100000001b3ull;
  }
  char out[17] = {};
  std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(hash));
  return out;
}

}  // namespace kcds::runtime::kcd1::xml
