// Declarative type/field schema — the single source of truth that drives
// wire codec, change-stamping, priority and authority, so client and server cannot drift.
//
// This runtime registry is the prototype form of the K2S_TYPE/FIELD macros in the spec; the macro
// sugar + constexpr field tables can wrap it later without changing the model.
#ifndef KCD2S_REPLICATION_SCHEMA_HPP
#define KCD2S_REPLICATION_SCHEMA_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kcd2s/core/ids.hpp"

namespace kcd2s::replication {

enum class FieldKind : std::uint8_t { F32, Vec3, Quat, I64, Bool, Str };
enum class Priority : std::uint8_t { Low = 0, Med = 1, High = 2 };

// AUTH_OWNER: owning client may write upstream, server relays. AUTH_SERVER: server-only, never
// echoed upstream.
enum class Authority : std::uint8_t { Server, Owner };

// Quantization params for the wire (bits==0 means "Dev/raw/unquantized"); consumed by the snapshot
// codec (Backend/core/quant) in a later slice.
struct Quant {
  float min = 0.0f;
  float max = 0.0f;
  int bits = 0;  // 0 => raw
};

struct FieldDef {
  core::FieldId id = 0;
  std::string name;
  FieldKind kind = FieldKind::F32;
  Priority prio = Priority::Med;
  Authority auth = Authority::Server;
  Quant quant;
};

struct TypeDef {
  core::TypeId id = 0;
  std::string name;
  std::vector<FieldDef> fields;  // indexed by FieldId

  static constexpr core::FieldId kNoField = std::numeric_limits<core::FieldId>::max();
  core::FieldId position_field = kNoField;  // the Vec3 "position" field, if any (interest hot path)

  std::size_t field_count() const {
    return fields.size();
  }
  const FieldDef* find(std::string_view n) const {
    for (const auto& f : fields)
      if (f.name == n)
        return &f;
    return nullptr;
  }
};

class SchemaRegistry {
 public:
  class TypeBuilder {
   public:
    TypeBuilder(SchemaRegistry& reg, TypeDef* def) : reg_(reg), def_(def) {
    }

    TypeBuilder& field(std::string name, FieldKind kind, Priority prio = Priority::Med,
                       Authority auth = Authority::Server, Quant quant = {}) {
      FieldDef f;
      f.id = static_cast<core::FieldId>(def_->fields.size());
      f.name = std::move(name);
      f.kind = kind;
      f.prio = prio;
      f.auth = auth;
      f.quant = quant;
      if (f.kind == FieldKind::Vec3 && f.name == "position")
        def_->position_field = f.id;
      def_->fields.push_back(std::move(f));
      return *this;
    }

    core::TypeId id() const {
      return def_->id;
    }

   private:
    SchemaRegistry& reg_;
    TypeDef* def_;
  };

  // Begin defining a type. The TypeId is crc32(name). Redefining a name replaces it.
  TypeBuilder define(std::string name) {
    const core::TypeId id = core::type_id(name);
    TypeDef def;
    def.id = id;
    def.name = std::move(name);
    TypeDef& stored = (types_[id] = std::move(def));
    return TypeBuilder(*this, &stored);
  }

  const TypeDef* get(core::TypeId id) const {
    auto it = types_.find(id);
    return it == types_.end() ? nullptr : &it->second;
  }
  const TypeDef* get(std::string_view name) const {
    return get(core::type_id(name));
  }
  std::size_t type_count() const {
    return types_.size();
  }

 private:
  std::unordered_map<core::TypeId, TypeDef> types_;
};

}  // namespace kcd2s::replication

#endif  // KCD2S_REPLICATION_SCHEMA_HPP
