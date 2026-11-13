#include "procedural_objects.hpp"

#include "placement.hpp"
#include "xml_text.hpp"

#include <string_view>

namespace kcds::runtime::kcd1 {
namespace {

using xml::attribute;
using xml::element_of;
using xml::has_attribute;
using xml::without_attribute;

// The element whose first tag is `tag` ("<Prefab", "<EntityPrototype") with Library="<library>"
// and Name="<name>", `full_name` being "<library>.<name>"; empty when there is none.
std::string library_entry(const std::string& text, std::string_view tag,
                          const std::string& full_name) {
  const std::size_t dot = full_name.find('.');
  if (dot == std::string::npos) {
    return {};
  }
  const std::string library = full_name.substr(0, dot);
  const std::string needle = "<" + std::string(tag) + " Name=\"" + full_name.substr(dot + 1) + "\"";
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    const std::size_t head_end = text.find('>', at);
    if (head_end == std::string::npos) {
      return {};
    }
    if (attribute(text.substr(at, head_end + 1 - at), "Library") == library) {
      return element_of(text, tag, at);
    }
    at = head_end;
  }
  return {};
}

// An entity of KCD2's level: its first tag's attributes, then its properties.
std::string entity_element(const std::string& attributes, const std::string& properties) {
  if (properties.empty()) {
    return "  <Entity " + attributes + " />\n";
  }
  return "  <Entity " + attributes + ">\n    " + properties + "\n  </Entity>\n";
}

}  // namespace

std::string procedural_object(const std::string& element, const FireLibraries& libraries,
                              long& next_id) {
  const std::string head = element.substr(0, element.find('>') + 1);
  const std::string variation =
      attribute(element_of(element, "ObjectVariation"), "sPrefabVariation");
  const std::string prefab = library_entry(libraries.prefabs, "Prefab", variation);
  if (prefab.empty()) {
    return {};
  }
  Vec3 position;
  if (!read_position(attribute(head, "Pos"), position)) {
    return {};
  }
  const Quat rotation = read_rotation(attribute(head, "Rotate"));
  const std::string name = attribute(head, "Name");
  const std::string guid = xml::guid_digits(attribute(head, "EntityGuid"));

  std::string out;
  std::size_t at = 0;
  while (true) {
    const std::size_t found = xml::tag_at(prefab.substr(at), "Object");
    if (found == std::string::npos) {
      break;
    }
    const std::string object = element_of(prefab, "Object", at + found);
    if (object.empty()) {
      break;
    }
    at += found + object.size();
    const std::string object_head = object.substr(0, object.find('>') + 1);
    const std::string type = attribute(object_head, "Type");

    // What the part is: a flame (its own properties) or a light (its archetype's).
    std::string kind;
    std::string properties;
    if (type == "ParticleEntity") {
      kind = attribute(object_head, "EntityClass");
      properties = element_of(object, "Properties");
    } else if (type == "EntityArchetype") {
      const std::string archetype = library_entry(
          libraries.archetypes, "EntityPrototype",
          attribute(object_head, "PrototypeLibrary") + "." + attribute(object_head, "Name"));
      kind = attribute(archetype.substr(0, archetype.find('>') + 1), "Class");
      properties = element_of(archetype, "Properties");
    }
    if (kind.empty() || properties.empty()) {
      continue;
    }

    Vec3 local;
    read_position(attribute(object_head, "Pos"), local);
    const Vec3 world = placed(position, rotation, local);
    const Quat turn = combined(rotation, read_rotation(attribute(object_head, "Rotate")));
    const std::string id = std::to_string(next_id++);
    const std::string part_guid =
        xml::derived_guid(guid + "/" + xml::lower(attribute(object_head, "Id")));
    std::string attributes = "Name=\"" + name + "_" + attribute(object_head, "Name") + "\" Pos=\"" +
                             written(world) + "\" Rotate=\"" + written(turn) + "\" EntityClass=\"" +
                             kind + "\" EntityId=\"" + id + "\" EntityGuid=\"" +
                             xml::guid_written(part_guid) + "\"";
    if (has_attribute(object_head, "ViewDistRatio")) {
      attributes += " ViewDistRatio=\"" + attribute(object_head, "ViewDistRatio") + "\"";
    }
    const std::string shadows = has_attribute(object_head, "CastShadowMinspec")
                                    ? attribute(object_head, "CastShadowMinspec")
                                    : std::string("1");
    attributes += " CastShadowMinSpec=\"" + shadows + "\"";
    out += entity_element(attributes, properties);
  }
  return out;
}

std::string with_archetype_properties(const std::string& element, const std::string& archetypes) {
  const std::size_t head_end = element.find('>');
  if (head_end == std::string::npos || head_end == 0 || element[head_end - 1] != '/') {
    // An entity with its own properties keeps them.
    return element;
  }
  const std::string head = element.substr(0, head_end + 1);
  const std::string archetype =
      library_entry(archetypes, "EntityPrototype", attribute(head, "Archetype"));
  const std::string properties = element_of(archetype, "Properties");
  if (properties.empty()) {
    return element;
  }
  std::string open = without_attribute(head, "Archetype");
  // "<Entity ... />" -> "<Entity ...>"
  const std::size_t slash = open.rfind('/');
  open = open.substr(0, slash) + ">";
  while (open.size() > 1 && open[open.size() - 2] == ' ') {
    open.erase(open.size() - 2, 1);
  }
  return open + "\n    " + properties + "\n  </Entity>" + element.substr(head_end + 1);
}

}  // namespace kcds::runtime::kcd1
