#include "kcd2s/bridge/apply.hpp"

#include <variant>

namespace kcd2s::bridge {

using replication::FieldKind;
using replication::Mirror;
using replication::SchemaRegistry;
using replication::TypeDef;

std::vector<TransformApply> extract_transforms(const SchemaRegistry& schema, const Mirror& mirror) {
  std::vector<TransformApply> out;
  out.reserve(mirror.size());

  for (const auto& [id, ent] : mirror.entities()) {
    const TypeDef* td = schema.get(ent.type);
    if (!td)
      continue;

    TransformApply a;
    a.entity = id;
    for (const auto& fd : td->fields) {
      if (fd.name == "position" && fd.kind == FieldKind::Vec3 && fd.id < ent.fields.size()) {
        if (const auto* p = std::get_if<core::Vec3>(&ent.fields[fd.id])) {
          a.position = *p;
          a.has_position = true;
        }
      } else if (fd.name == "rotation" && fd.kind == FieldKind::Quat && fd.id < ent.fields.size()) {
        if (const auto* q = std::get_if<core::Quat>(&ent.fields[fd.id])) {
          a.rotation = *q;
          a.has_rotation = true;
        }
      }
    }
    if (a.has_position || a.has_rotation)
      out.push_back(a);
  }
  return out;
}

}  // namespace kcd2s::bridge
