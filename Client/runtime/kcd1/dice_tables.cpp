#include "dice_tables.hpp"

#include "placement.hpp"
#include "xml_text.hpp"

#include <cmath>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace kcds::runtime::kcd1 {
namespace {

using xml::attribute;
using xml::each_entity;
using xml::guid_digits;
using xml::guid_written;
using xml::has_attribute;
using xml::lower;
using xml::with_attribute;
using xml::without_attribute;

std::optional<Vec3> position(const std::string& head) {
  Vec3 at;
  if (!has_attribute(head, "Pos") || !read_position(attribute(head, "Pos"), at)) {
    return std::nullopt;
  }
  return at;
}

Quat rotation(const std::string& head) {
  return read_rotation(attribute(head, "Rotate"));
}

// The prefab instance in a name's brackets, "[Table/table_dice1_<GUID>]".
std::string bracket(const std::string& name) {
  const std::size_t open = name.rfind('[');
  return open == std::string::npos ? std::string() : name.substr(open);
}

struct TemplateEntity {
  std::string element;
  std::string head;
  std::string kind;
  std::string id;
  std::string guid;  // sixteen hex digits
};

struct Template {
  std::vector<TemplateEntity> entities;
  std::string bracket;
  Vec3 seat1;
  Vec3 seat2;
};

std::optional<Template> find_template(const std::string& kcd2_objects) {
  Template found;
  each_entity(kcd2_objects, [&](const std::string&, const std::string& head) {
    if (found.bracket.empty() && attribute(head, "EntityGuid") == kDiceTemplateInteractor) {
      found.bracket = bracket(attribute(head, "Name"));
    }
  });
  if (found.bracket.empty()) {
    return std::nullopt;
  }
  bool seat1 = false;
  bool seat2 = false;
  each_entity(kcd2_objects, [&](const std::string& element, const std::string& head) {
    const std::string name = attribute(head, "Name");
    if (bracket(name) != found.bracket) {
      return;
    }
    found.entities.push_back({element, head, attribute(head, "EntityClass"),
                              attribute(head, "EntityId"),
                              guid_digits(attribute(head, "EntityGuid"))});
    const auto at = position(head);
    if (at && name.rfind("seat1_SO[", 0) == 0) {
      found.seat1 = *at;
      seat1 = true;
    }
    if (at && name.rfind("seat2_SO[", 0) == 0) {
      found.seat2 = *at;
      seat2 = true;
    }
  });
  if (!seat1 || !seat2) {
    return std::nullopt;
  }
  return found;
}

// A KCD1 table: its prefab instance GUID and its two seats.
struct Kcd1Table {
  std::string instance;
  std::optional<Vec3> seat1;
  std::optional<Vec3> seat2;
};

std::vector<Kcd1Table> kcd1_tables(const std::string& kcd1_objects) {
  std::map<std::string, Kcd1Table> by_instance;
  constexpr std::string_view kCup = "dice_cup_";
  each_entity(kcd1_objects, [&](const std::string&, const std::string& head) {
    const std::string name = attribute(head, "Name");
    if (attribute(head, "EntityClass") == "DiceMinigameCup" && name.rfind(kCup, 0) == 0) {
      const std::string instance = lower(name.substr(kCup.size()));
      by_instance[instance].instance = instance;
    }
  });
  each_entity(kcd1_objects, [&](const std::string&, const std::string& head) {
    if (attribute(head, "EntityClass") != "SmartObjectHolder") {
      return;
    }
    const std::string name = lower(attribute(head, "Name"));
    for (auto& [instance, table] : by_instance) {
      if (name == "seat1_" + instance) {
        table.seat1 = position(head);
      } else if (name == "seat2_" + instance) {
        table.seat2 = position(head);
      }
    }
  });
  std::vector<Kcd1Table> tables;
  for (const auto& [instance, table] : by_instance) {
    if (table.seat1 && table.seat2) {
      tables.push_back(table);
    }
  }
  return tables;
}

// The turn and move that take the template's seats to a KCD1 table's: about z by `angle`
// around the template's seats' middle, which then stands at the KCD1 seats' middle.
struct Placement {
  Vec3 from;
  Vec3 to;
  double angle = 0;

  Vec3 moved(const Vec3& at) const {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double dx = at.x - from.x;
    const double dy = at.y - from.y;
    return {to.x + c * dx - s * dy, to.y + s * dx + c * dy, to.z + at.z - from.z};
  }

  Quat turned(const Quat& q) const {
    const double c = std::cos(angle / 2);
    const double s = std::sin(angle / 2);
    return {c * q.w - s * q.z, c * q.x - s * q.y, c * q.y + s * q.x, c * q.z + s * q.w};
  }
};

Vec3 middle(const Vec3& a, const Vec3& b) {
  return {(a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2};
}

Placement placement(const Template& from, const Kcd1Table& to) {
  Placement out;
  out.from = middle(from.seat1, from.seat2);
  out.to = middle(*to.seat1, *to.seat2);
  const double template_heading =
      std::atan2(from.seat2.y - from.seat1.y, from.seat2.x - from.seat1.x);
  const double table_heading = std::atan2(to.seat2->y - to.seat1->y, to.seat2->x - to.seat1->x);
  // The shorter way round, between -180 and 180 degrees.
  constexpr double kPi = 3.14159265358979323846;
  out.angle = std::remainder(table_heading - template_heading, 2 * kPi);
  return out;
}

// Every <Link> of an element with its target's id and GUID as the placed table's.
std::string relinked(const std::string& element, const std::map<std::string, std::string>& ids,
                     const std::map<std::string, std::string>& guids) {
  std::string out;
  std::size_t from = 0;
  std::size_t at = 0;
  while ((at = element.find("<Link ", from)) != std::string::npos) {
    const std::size_t end = element.find('>', at);
    if (end == std::string::npos) {
      break;
    }
    std::string tag = element.substr(at, end + 1 - at);
    const auto id = ids.find(attribute(tag, "TargetId"));
    if (id != ids.end()) {
      tag = with_attribute(tag, "TargetId", id->second);
    }
    const auto guid = guids.find(guid_digits(attribute(tag, "TargetGuid")));
    if (guid != guids.end()) {
      tag = with_attribute(tag, "TargetGuid", guid_written(guid->second));
    }
    out += element.substr(from, at - from) + tag;
    from = end + 1;
  }
  return out + element.substr(from);
}

std::string placed_table(const Template& from, const Kcd1Table& table, long& next_id) {
  const Placement place = placement(from, table);
  std::map<std::string, std::string> ids;
  std::map<std::string, std::string> guids;
  for (const TemplateEntity& entity : from.entities) {
    ids[entity.id] = std::to_string(next_id++);
    guids[entity.guid] = dice_table_guid(table.instance, entity.guid);
  }
  const std::string instance_bracket = "[Table/table_dice_kcd1_" + table.instance + "]";
  std::string out;
  for (const TemplateEntity& entity : from.entities) {
    // KCD1's table keeps its own light.
    if (entity.kind == "Light") {
      continue;
    }
    std::string head = without_attribute(entity.head, "EditorLayer");
    const std::string name = attribute(head, "Name");
    head = with_attribute(head, "Name", name.substr(0, name.rfind('[')) + instance_bracket);
    head = with_attribute(head, "EntityId", ids[entity.id]);
    head = with_attribute(head, "EntityGuid", guid_written(guids[entity.guid]));
    if (has_attribute(head, "ParentId")) {
      // A child stands where its parent does, as it did in the template.
      const auto parent = ids.find(attribute(head, "ParentId"));
      if (parent != ids.end()) {
        head = with_attribute(head, "ParentId", parent->second);
      }
    } else {
      const auto at = position(head);
      if (at) {
        head = with_attribute(head, "Pos", written(place.moved(*at)));
      }
      head = with_attribute(head, "Rotate", written(place.turned(rotation(head))));
    }
    const std::string body = entity.element.substr(entity.head.size());
    out += "  " + relinked(head + body, ids, guids) + "\n";
  }
  return out;
}

}  // namespace

std::string dice_table_guid(const std::string& instance, const std::string& template_guid) {
  return xml::derived_guid(lower(instance) + "/" + guid_digits(template_guid));
}

std::string dice_tables(const std::string& kcd1_objects, const std::string& kcd2_objects,
                        long& next_id, std::size_t& tables) {
  // Most of the objects given (a layer's) have no table, so those are looked for first.
  const auto kcd1 = kcd1_tables(kcd1_objects);
  if (kcd1.empty()) {
    return {};
  }
  const auto from = find_template(kcd2_objects);
  if (!from) {
    return {};
  }
  std::string out;
  for (const Kcd1Table& table : kcd1) {
    out += placed_table(*from, table, next_id);
    tables += 1;
  }
  return out;
}

}  // namespace kcds::runtime::kcd1
