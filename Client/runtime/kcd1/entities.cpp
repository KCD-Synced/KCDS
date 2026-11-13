#include "entities.hpp"

#include "dice_tables.hpp"
#include "xml_text.hpp"

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
// The ids of the entities a bed adds (KCD1's highest, its layers' included, is below 130000).
constexpr long kFirstNewId = 900000;

using xml::attribute;
using xml::contains;
using xml::has_attribute;
using xml::lower;
using xml::with_attribute;
using xml::with_tag;
using xml::without_attribute;

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

// An entity of a class both games share as it is (KCD1's smart object attributes left out).
std::string as_it_is(const std::string& element) {
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

// A rotation ("w,x,y,z") followed by -90 degrees about its own z axis.
std::string quarter_turned(const std::string& rotate) {
  double w = 1;
  double x = 0;
  double y = 0;
  double z = 0;
  std::sscanf(rotate.c_str(), "%lf,%lf,%lf,%lf", &w, &x, &y, &z);
  const double c = std::sqrt(0.5);
  const double s = -std::sqrt(0.5);
  char out[128] = {};
  std::snprintf(out, sizeof(out), "%g,%g,%g,%g", w * c - z * s, x * c + y * s, y * c - x * s,
                z * c + w * s);
  return out;
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

  // The place to lie on turned a quarter (-90 degrees about z) from the bed: KCD1's bed helpers
  // have the sides along the bed's y axis and its length along x, KCD2's the sides along x. Off
  // by that quarter the game finds no way into the bed and the player stays standing.
  const std::string place_rotate = quarter_turned(rotate);

  std::string out;
  if (!model.empty()) {
    out += "  <Entity Name=\"" + name + "\" Pos=\"" + pos + "\" Rotate=\"" + rotate +
           "\" Scale=\"" + scale + "\" EntityClass=\"GeomEntity\" EntityId=\"" + id +
           "\" EntityGuid=\"" + entity_guid + "\" CastShadowMinSpec=\"1\" Geometry=\"" + model +
           "\">\n    <Properties bSaved_by_game=\"0\" />\n  </Entity>\n";
  }
  out += "  <Entity Name=\"" + name + "_bed\" Pos=\"" + pos + "\" Rotate=\"" + place_rotate +
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

std::string translate_objects(const std::string& kcd1_objects, const ObjectSources& sources,
                              EntityCounts& counts) {
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
    } else if (kind == "Light") {
      translated = as_it_is(with_archetype_properties(element, sources.fires.archetypes));
    } else if (kind == "Chair" || kind == "ReflexLight" || kind == "EnvironmentLight" ||
               kind == "ParticleEffect") {
      translated = as_it_is(element);
    } else if (kind == "Bed") {
      out += bed(element, next_id);
      counts.kept[kind] += 1;
      continue;
    } else if (kind == "ProceduralObject") {
      const std::string parts = procedural_object(element, sources.fires, next_id);
      if (parts.empty()) {
        counts.left_out += 1;
      } else {
        out += parts;
        counts.kept[kind] += 1;
      }
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
  std::size_t tables = 0;
  out += dice_tables(kcd1_objects, sources.kcd2_objects, next_id, tables);
  if (tables > 0) {
    counts.kept["DiceTable"] += tables;
  }
  out += "</Objects>\n";
  return out;
}

}  // namespace kcds::runtime::kcd1
