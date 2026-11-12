#include "entities.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace kcds::runtime::kcd1 {
namespace {

constexpr char kDoorType[] = "49515779-1a9d-330c-4768-c9326ea1ca96";
constexpr char kLadderType[] = "450aabee-95e7-4ff6-eca8-3d783b1664ae";
constexpr char kBedType[] = "425d4fdf-8dcd-4a2b-fdc5-cbb1b5d25b89";
constexpr int kLadderHeights[] = {225, 250, 300, 350, 400};
// The ids of the entities a bed adds (KCD1's highest is below 70000).
constexpr long kFirstNewId = 100000;

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

// The tag with the attribute set (added before the tag's end when it has none).
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

// The element with its first <name ...> tag changed by `edit`.
template <typename Edit>
std::string with_tag(const std::string& element, std::string_view name, Edit edit) {
  const std::string open = "<" + std::string(name);
  std::size_t at = 0;
  while ((at = element.find(open, at)) != std::string::npos) {
    const char next = at + open.size() < element.size() ? element[at + open.size()] : '\0';
    if (next == ' ' || next == '>' || next == '/') {
      break;
    }
    at += open.size();
  }
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

// KCD1's GUID (16 hex digits) as KCD2 writes it; anything else as it is.
std::string guid(const std::string& value) {
  if (value.size() != 16 || !std::all_of(value.begin(), value.end(),
                                         [](char c) { return std::isxdigit(unsigned(c)) != 0; })) {
    return value;
  }
  const std::string digits = lower(value);
  return digits.substr(0, 8) + "-" + digits.substr(8, 4) + "-" + digits.substr(12);
}

// The GUID of an entity added beside another (its first digit replaced).
std::string other_guid(const std::string& value, char digit) {
  if (value.empty()) {
    return value;
  }
  std::string out = value;
  out[0] = digit;
  return out;
}

// One of KCD2's generated inventories for a stash's model.
const char* inventory(const std::string& model) {
  const std::string name = lower(model);
  if (contains(name, "chest")) {
    return "inventory_household";
  }
  if (contains(name, "sack") || contains(name, "basket") || contains(name, "barrel")) {
    return "inventory_defaultJunk_sack";
  }
  if (contains(name, "shelve") || contains(name, "supplies")) {
    return "inventory_defaultJunk_shelf";
  }
  return "inventory_defaultJunk";
}

std::string properties_kcd2(const std::string& tag) {
  return without_attribute(without_attribute(tag, "soclasses_SmartObjectHelpers"),
                           "soclasses_SmartObjectClass");
}

std::string door(const std::string& element) {
  const std::string helpers = lower(attribute(element, "soclasses_SmartObjectHelpers"));
  const std::string model = lower(attribute(element, "object_Model"));
  const bool right = helpers.empty() ? contains(model, "right") : contains(helpers, "right");
  std::string out = with_tag(element, "Properties", [right](const std::string& tag) {
    std::string props = properties_kcd2(tag);
    props = with_attribute(props, "esDoorAnimSet", right ? "VillageRight" : "VillageLeft");
    props = with_attribute(props, "soclass_SmartObjectHelpers", right ? "DoorRight" : "DoorLeft");
    return with_attribute(props, "guidSmartObjectType", kDoorType);
  });
  const std::string from = attribute(element, "fKeepOpenFrom");
  const std::string until = attribute(element, "fKeepOpenUntil");
  return with_tag(out, "Lock", [&](const std::string& tag) {
    std::string lock = tag;
    if (!from.empty()) {
      lock = with_attribute(lock, "fKeepUnlockedFrom", from);
    }
    if (!until.empty()) {
      lock = with_attribute(lock, "fKeepUnlockedTo", until);
    }
    return lock;
  });
}

std::string stash(const std::string& element) {
  const std::string model = attribute(element, "object_Model");
  const std::string out = with_tag(element, "Properties", [](const std::string& tag) {
    return without_attribute(properties_kcd2(tag), "guidSmartObjectType");
  });
  return with_tag(out, "Database", [&model](const std::string& tag) {
    return with_attribute(without_attribute(tag, "guidInventoryDBId"), "sGeneratedInventory",
                          inventory(model));
  });
}

std::string ladder(const std::string& element) {
  const std::string height = attribute(element, "height");
  const double centimetres = (height.empty() ? 3.0 : std::atof(height.c_str())) * 100.0;
  int best = kLadderHeights[0];
  for (const int candidate : kLadderHeights) {
    if (std::abs(candidate - centimetres) < std::abs(best - centimetres)) {
      best = candidate;
    }
  }
  return with_tag(element, "Properties", [best](const std::string& tag) {
    std::string props = properties_kcd2(tag);
    props = with_attribute(props, "soclass_SmartObjectHelpers", "Ladder_" + std::to_string(best));
    return with_attribute(props, "guidSmartObjectType", kLadderType);
  });
}

std::string chair(const std::string& element) {
  return with_tag(element, "Properties", [](const std::string& tag) {
    return without_attribute(properties_kcd2(tag), "guidSmartObjectType");
  });
}

// A bed's kind in KCD2: its smart object helper, bed type and qualities.
struct BedKind {
  const char* helper;
  const char* type;
  const char* reading;
  const char* sleep;
};

BedKind bed_kind(const std::string& kcd1_helpers) {
  const std::string helpers = lower(kcd1_helpers);
  if (helpers == "bed") {
    return {"Bed_1Place_High", "NormalBed", "bed_normal", "medium"};
  }
  if (contains(helpers, "sleepingbench")) {
    return {"Bed_1Place_Bench", "BenchBed", "bed_ground", "low"};
  }
  if (contains(helpers, "shelter") || contains(helpers, "tent")) {
    return {"Bed_1Place_Tent", "TentBed", "bed_ground", "low"};
  }
  return {"Bed_1Place_Low", "GroundBed", "bed_ground", "low"};
}

std::string bed(const std::string& element, long& next_id) {
  const std::string head = element.substr(0, element.find('>') + 1);
  const std::string name = attribute(head, "Name");
  const std::string pos = attribute(head, "Pos");
  const std::string rotate = has_attribute(head, "Rotate") ? attribute(head, "Rotate") : "1,0,0,0";
  const std::string scale = has_attribute(head, "Scale") ? attribute(head, "Scale") : "1,1,1";
  const std::string id = attribute(head, "EntityId");
  const std::string entity_guid = guid(attribute(head, "EntityGuid"));
  const std::string model = attribute(element, "object_Model");
  const BedKind kind = bed_kind(attribute(element, "soclasses_SmartObjectHelpers"));
  const std::string place_id = std::to_string(next_id);
  const std::string trigger_id = std::to_string(next_id + 1);
  next_id += 2;
  const std::string place_guid = other_guid(entity_guid, 'b');
  const std::string trigger_guid = other_guid(entity_guid, 'c');

  // The trigger a little above the bed's origin, where the player looks at it.
  double x = 0;
  double y = 0;
  double z = 0;
  std::sscanf(pos.c_str(), "%lf,%lf,%lf", &x, &y, &z);
  char trigger_pos[96] = {};
  std::snprintf(trigger_pos, sizeof(trigger_pos), "%g,%g,%g", x, y, z + 0.4);

  std::string out;
  if (!model.empty()) {
    out += "  <Entity Name=\"" + name + "\" Pos=\"" + pos + "\" Rotate=\"" + rotate +
           "\" Scale=\"" + scale + "\" EntityClass=\"GeomEntity\" EntityId=\"" + id +
           "\" EntityGuid=\"" + entity_guid + "\" CastShadowMinSpec=\"1\" Geometry=\"" + model +
           "\">\n    <Properties bSaved_by_game=\"0\" />\n  </Entity>\n";
  }
  out += "  <Entity Name=\"" + name + "_bed\" Pos=\"" + pos + "\" Rotate=\"" + rotate +
         "\" EntityClass=\"StanceSmartObject\" EntityId=\"" + place_id + "\" EntityGuid=\"" +
         place_guid + "\" CastShadowMinSpec=\"1\">\n    <EntityLinks>\n      <Link TargetId=\"" +
         trigger_id +
         "\" TargetGuid=\"00000000-0000-0000\" Name=\"mTrigger\" />\n    </EntityLinks>\n"
         "    <Properties bOwnedByHome=\"0\" soclass_SmartObjectHelpers=\"" +
         kind.helper + "\" guidSmartObjectType=\"" + kBedType +
         "\" sWH_AI_EntityCategory=\"Bed\" bSaved_by_game=\"0\">\n      <Bed esReadingQuality=\"" +
         kind.reading + "\" esSleepQuality=\"" + kind.sleep + "\" />\n      <Script esBedTypes=\"" +
         kind.type + "\" />\n    </Properties>\n  </Entity>\n";
  out += "  <Entity Name=\"" + name + "_trigger\" Pos=\"" + trigger_pos + "\" Rotate=\"" + rotate +
         "\" Scale=\"0.39,0.39,0.39\" EntityClass=\"BedTrigger\" EntityId=\"" + trigger_id +
         "\" EntityGuid=\"" + trigger_guid +
         "\" CastShadowMinSpec=\"1\">\n    <EntityLinks>\n      <Link TargetId=\"" + place_id +
         "\" TargetGuid=\"" + place_guid +
         "\" Name=\"\" />\n    </EntityLinks>\n"
         "    <Properties InteractorPriorityOverride=\"1\">\n"
         "      <Click sAction=\"lying\" bAllowNoOwner=\"1\" bCheckOwner=\"0\" "
         "UseMessage=\"@ui_hud_sleep_and_save\" UseNotOwnerMessage=\"@ui_hud_sleep\" />\n"
         "    </Properties>\n  </Entity>\n";
  return out;
}

// The top-level elements' starts (each "  <" and a letter at the start of a line).
std::vector<std::size_t> element_starts(const std::string& text) {
  std::vector<std::size_t> starts;
  std::size_t at = 0;
  while ((at = text.find("\n  <", at)) != std::string::npos) {
    const std::size_t start = at + 1;
    const char next = start + 3 < text.size() ? text[start + 3] : '\0';
    if (std::isalpha(unsigned(next))) {
      starts.push_back(start);
    }
    at = start;
  }
  return starts;
}

}  // namespace

std::string translate_objects(const std::string& kcd1_objects, EntityCounts& counts) {
  std::string out = "<Objects>\n";
  const std::vector<std::size_t> starts = element_starts(kcd1_objects);
  std::size_t end_of_list = kcd1_objects.rfind("</Objects>");
  if (end_of_list == std::string::npos) {
    end_of_list = kcd1_objects.size();
  }
  long next_id = kFirstNewId;
  for (std::size_t index = 0; index < starts.size(); ++index) {
    const std::size_t start = starts[index];
    const std::size_t end = index + 1 < starts.size() ? starts[index + 1] : end_of_list;
    if (end <= start) {
      continue;
    }
    std::string element = kcd1_objects.substr(start, end - start);
    const std::string head = element.substr(0, element.find('>') + 1);
    if (head.rfind("  <Entity", 0) != 0) {
      counts.left_out += 1;
      continue;
    }
    const std::string kind = attribute(head, "EntityClass");
    std::string translated;
    if (kind == "AnimDoor") {
      translated = door(element);
    } else if (kind == "Stash") {
      translated = stash(element);
    } else if (kind == "Ladder") {
      translated = ladder(element);
    } else if (kind == "Chair") {
      translated = chair(element);
    } else if (kind == "Bed") {
      out += bed(element, next_id);
      counts.kept[kind] += 1;
      continue;
    } else {
      counts.left_out += 1;
      continue;
    }
    // Every kept entity's GUID as KCD2 writes it.
    const std::size_t head_end = translated.find('>') + 1;
    const std::string new_head =
        with_attribute(translated.substr(0, head_end), "EntityGuid",
                       guid(attribute(translated.substr(0, head_end), "EntityGuid")));
    out += new_head + translated.substr(head_end);
    counts.kept[kind] += 1;
  }
  out += "</Objects>\n";
  return out;
}

}  // namespace kcds::runtime::kcd1
