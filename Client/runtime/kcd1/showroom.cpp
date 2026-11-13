#include "showroom.hpp"

#include "xml_text.hpp"

#include <map>
#include <vector>

namespace kcds::runtime::kcd1 {
namespace {

using xml::attribute;
using xml::each_entity;
using xml::has_attribute;
using xml::with_attribute;

constexpr char kShowroomLayer[] = "showroom";

struct RoomEntity {
  std::string element;
  std::string head;
};

// Every <Link> of an element with its target's id as the KCD1 map has it.
std::string relinked(const std::string& element, const std::map<std::string, std::string>& ids) {
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
    out += element.substr(from, at - from) + tag;
    from = end + 1;
  }
  return out + element.substr(from);
}

}  // namespace

std::string showroom(const std::string& kcd2_objects, long& next_id, std::size_t& count) {
  std::vector<RoomEntity> room;
  each_entity(kcd2_objects, [&](const std::string& element, const std::string& head) {
    if (attribute(head, "EditorLayer") == kShowroomLayer) {
      room.push_back({element, head});
    }
  });
  std::map<std::string, std::string> ids;
  for (const RoomEntity& entity : room) {
    ids[attribute(entity.head, "EntityId")] = std::to_string(next_id++);
  }
  std::string out;
  for (const RoomEntity& entity : room) {
    std::string head =
        with_attribute(entity.head, "EntityId", ids[attribute(entity.head, "EntityId")]);
    if (has_attribute(head, "ParentId")) {
      const auto parent = ids.find(attribute(head, "ParentId"));
      if (parent != ids.end()) {
        head = with_attribute(head, "ParentId", parent->second);
      }
    }
    const std::string body = entity.element.substr(entity.head.size());
    out += "  " + relinked(head + body, ids) + "\n";
  }
  count = room.size();
  return out;
}

}  // namespace kcds::runtime::kcd1
