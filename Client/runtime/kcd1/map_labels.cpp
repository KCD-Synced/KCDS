#include "map_labels.hpp"

#include <cmath>
#include <cstdlib>
#include <string_view>

#include "xml_text.hpp"

namespace kcds::runtime::kcd1 {
namespace {

using xml::attribute;
using xml::lower;

constexpr double kWorldSize = 4096.0;
constexpr double kPixelsPerMetre = 2.0;
constexpr char kTextPrefix[] = "kcds_kcd1_";
constexpr char kHeader[] =
    "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\n"
    "<database xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" name=\"barbora\" "
    "xsi:noNamespaceSchemaLocation=\"../database.xsd\">\n";

struct Label {
  std::string location;
  std::string id;
  std::string name;  // KCD1's text key ("location_Rataje")
  double x = 0;
  double y = 0;
};

// KCD1's labels that have a place on its map.
std::vector<Label> labels_of(const std::string& kcd1_labels) {
  std::vector<Label> labels;
  std::size_t at = 0;
  while ((at = kcd1_labels.find("<row ", at)) != std::string::npos) {
    const std::size_t end = kcd1_labels.find('>', at);
    if (end == std::string::npos) {
      break;
    }
    const std::string_view tag(kcd1_labels.data() + at, end + 1 - at);
    at = end;
    Label label;
    label.location = attribute(tag, "location_id");
    label.id = attribute(tag, "ui_map_label_id");
    label.name = attribute(tag, "ui_name");
    label.x = std::atof(attribute(tag, "position_x").c_str());
    label.y = std::atof(attribute(tag, "position_y").c_str());
    if (label.location.empty() || label.id.empty() || label.name.empty() || label.x < 0 ||
        label.y < 0 || label.x > kWorldSize || label.y > kWorldSize) {
      continue;
    }
    labels.push_back(label);
  }
  return labels;
}

std::string text_key(const Label& label) {
  return kTextPrefix + lower(label.name);
}

std::string pixels(double value) {
  return std::to_string(static_cast<long>(std::lround(value * kPixelsPerMetre)));
}

// The last cell of the text table's row of `key`, empty when it has none.
std::string text_of(const std::string& texts, const std::string& key) {
  const std::size_t found = texts.find("<Cell>" + key + "</Cell>");
  if (found == std::string::npos) {
    return {};
  }
  const std::size_t row_end = texts.find("</Row>", found);
  std::string last;
  std::size_t at = found;
  while ((at = texts.find("<Cell>", at)) != std::string::npos && at < row_end) {
    const std::size_t start = at + std::char_traits<char>::length("<Cell>");
    const std::size_t end = texts.find("</Cell>", start);
    if (end == std::string::npos || end > row_end) {
      break;
    }
    if (end > start) {
      last = texts.substr(start, end - start);
    }
    at = end;
  }
  return last;
}

}  // namespace

std::string location_patch(const std::string& kcd1_labels) {
  std::string out = std::string(kHeader) + "\t<locations version=\"1\">\n";
  for (const Label& label : labels_of(kcd1_labels)) {
    out += "\t\t<location level_id=\"0\" location_category_id=\"1\" location_id=\"" +
           label.location + "\" location_name=\"" + text_key(label) + "\" ui_type=\"Unique\" />\n";
  }
  return out + "\t</locations>\n</database>\n";
}

std::string label_patch(const std::string& kcd1_labels) {
  std::string out = std::string(kHeader) + "\t<ui_map_labels version=\"1\">\n";
  for (const Label& label : labels_of(kcd1_labels)) {
    out += "\t\t<ui_map_label location_id=\"" + label.location + "\" position_x=\"" +
           pixels(label.x) + "\" position_y=\"" + pixels(kWorldSize - label.y) +
           "\" ui_map_label_id=\"" + label.id + "\" ui_name=\"" + text_key(label) + "\" />\n";
  }
  return out + "\t</ui_map_labels>\n</database>\n";
}

std::vector<std::pair<std::string, std::string>> label_texts(const std::string& kcd1_labels,
                                                             const std::string& kcd1_texts) {
  std::vector<std::pair<std::string, std::string>> texts;
  for (const Label& label : labels_of(kcd1_labels)) {
    std::string text = text_of(kcd1_texts, label.name);
    if (text.empty()) {
      // "location_Kunuv_tabor" -> "Kunuv tabor"
      text = label.name.substr(label.name.find('_') + 1);
      for (char& c : text) {
        c = c == '_' ? ' ' : c;
      }
    }
    texts.emplace_back(text_key(label), text);
  }
  return texts;
}

}  // namespace kcds::runtime::kcd1
