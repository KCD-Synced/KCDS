#include "layer_catalog.hpp"

#include <cctype>
#include <cstdlib>

namespace kcds::runtime::level::layers {
namespace {

// The value of attribute `name` in the tag starting at `tag` (up to its closing '>'), or nullopt.
std::optional<std::string_view> attribute(std::string_view text, std::size_t tag,
                                          std::string_view name) {
  const std::size_t end = text.find('>', tag);
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view element = text.substr(tag, end - tag);
  std::size_t at = 0;
  while ((at = element.find(name, at)) != std::string_view::npos) {
    const bool starts = at > 0 && std::isspace(static_cast<unsigned char>(element[at - 1]));
    const std::size_t equals = at + name.size();
    if (starts && equals + 1 < element.size() && element[equals] == '=' &&
        element[equals + 1] == '"') {
      const std::size_t value = equals + 2;
      const std::size_t close = element.find('"', value);
      if (close == std::string_view::npos) {
        return std::nullopt;
      }
      return element.substr(value, close - value);
    }
    at = equals;
  }
  return std::nullopt;
}

bool hex(char c) {
  return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

// Whether `text` is a GUID: 8-4-4-4-12 hex digits.
bool guid(std::string_view text) {
  constexpr std::size_t kGroups[] = {8, 4, 4, 4, 12};
  std::size_t at = 0;
  for (std::size_t group = 0; group < 5; ++group) {
    for (std::size_t i = 0; i < kGroups[group]; ++i, ++at) {
      if (at >= text.size() || !hex(text[at])) {
        return false;
      }
    }
    if (group < 4) {
      if (at >= text.size() || text[at] != '-') {
        return false;
      }
      ++at;
    }
  }
  return at == text.size();
}

}  // namespace

std::vector<CatalogLayer> parse_level_layers(std::string_view leveldata) {
  std::vector<CatalogLayer> found;
  std::size_t at = 0;
  while ((at = leveldata.find("<Layer ", at)) != std::string_view::npos) {
    const auto name = attribute(leveldata, at, "Name");
    const auto id = attribute(leveldata, at, "Id");
    at += 7;
    if (!name || !id || name->empty() || id->empty()) {
      continue;
    }
    const long value = std::strtol(std::string(*id).c_str(), nullptr, 10);
    if (value <= 0 || value > 0xFFFF) {
      continue;
    }
    found.push_back({std::uint16_t(value), std::string(*name)});
  }
  return found;
}

std::string short_layer_name(std::string_view name) {
  constexpr std::size_t kGuid = 36;
  if (name.size() > kGuid + 1 && name[name.size() - kGuid - 1] == '_' &&
      guid(name.substr(name.size() - kGuid))) {
    return std::string(name.substr(0, name.size() - kGuid - 1));
  }
  return std::string(name);
}

std::optional<LayerPlace> parse_layer_file(std::string_view xml) {
  LayerPlace place;
  double sum[3] = {0.0, 0.0, 0.0};
  std::size_t at = 0;
  while ((at = xml.find("<Entity ", at)) != std::string_view::npos) {
    const auto pos = attribute(xml, at, "Pos");
    if (place.path.empty()) {
      if (const auto path = attribute(xml, at, "EditorLayer")) {
        place.path = std::string(*path);
      }
    }
    at += 8;
    if (!pos) {
      continue;
    }
    const std::string text(*pos);
    char* end = nullptr;
    const double x = std::strtod(text.c_str(), &end);
    if (!end || *end != ',') {
      continue;
    }
    const double y = std::strtod(end + 1, &end);
    if (!end || *end != ',') {
      continue;
    }
    const double z = std::strtod(end + 1, &end);
    sum[0] += x;
    sum[1] += y;
    sum[2] += z;
    ++place.entities;
  }
  if (place.entities == 0) {
    return std::nullopt;
  }
  for (int axis = 0; axis < 3; ++axis) {
    place.centre[axis] = float(sum[axis] / double(place.entities));
  }
  return place;
}

}  // namespace kcds::runtime::level::layers
