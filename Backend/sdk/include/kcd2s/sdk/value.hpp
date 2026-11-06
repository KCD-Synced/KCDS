// Canonical cross-language value model. Every runtime (JS, TS, Lua, C#)
// converts to/from this one type, so a value emitted from one language arrives sane in another and
// on the network. This is the single value currency of the SDK's events, RPC, exports and state.
#ifndef KCD2S_SDK_VALUE_HPP
#define KCD2S_SDK_VALUE_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kcd2s/core/ids.hpp"

namespace kcd2s::sdk {

enum class Tag : std::uint8_t {
  Null = 0,
  Bool = 1,
  Int = 2,     // signed 64-bit
  Double = 3,  // IEEE 754 double
  Str = 4,     // UTF-8
  Bytes = 5,   // opaque byte blob
  Array = 6,
  Map = 7,        // ordered, string keys
  EntityRef = 8,  // marshals to the right facade object per language
  FuncRef = 9,    // a callback living in some resource (for cross-language callbacks)
};

// An EntityId wrapped so it is distinct from a plain Int on the wire and in the variant.
struct EntityRef {
  core::EntityId id = 0;
  friend bool operator==(const EntityRef& a, const EntityRef& b) {
    return a.id == b.id;
  }
};

// Identifies a callback: which resource owns it and its per-resource id. The host keeps a ref table
// per resource and invalidates on stop (no leaks across unload).
struct FuncRef {
  std::uint32_t resource = 0;
  std::uint32_t id = 0;
  friend bool operator==(const FuncRef& a, const FuncRef& b) {
    return a.resource == b.resource && a.id == b.id;
  }
};

class Value {
 public:
  using Bytes = std::vector<std::uint8_t>;
  using Array = std::vector<Value>;
  using MapEntry = std::pair<std::string, Value>;
  using Map = std::vector<MapEntry>;

  Value() : v_(std::monostate{}) {
  }
  Value(bool b) : v_(b) {
  }
  Value(std::int64_t i) : v_(i) {
  }
  Value(int i) : v_(static_cast<std::int64_t>(i)) {
  }
  Value(double d) : v_(d) {
  }
  Value(const char* s) : v_(std::string(s)) {
  }
  Value(std::string s) : v_(std::move(s)) {
  }
  Value(Bytes b) : v_(std::move(b)) {
  }
  Value(Array a) : v_(std::move(a)) {
  }
  Value(Map m) : v_(std::move(m)) {
  }
  Value(EntityRef e) : v_(e) {
  }
  Value(FuncRef f) : v_(f) {
  }

  static Value null() {
    return Value{};
  }
  static Value bytes(Bytes b) {
    return Value{std::move(b)};
  }

  Tag tag() const {
    return static_cast<Tag>(v_.index());
  }
  bool is_null() const {
    return tag() == Tag::Null;
  }

  // Typed accessors (undefined if the tag does not match; check tag() first).
  bool as_bool() const {
    return std::get<bool>(v_);
  }
  std::int64_t as_int() const {
    return std::get<std::int64_t>(v_);
  }
  double as_double() const {
    return std::get<double>(v_);
  }
  const std::string& as_str() const {
    return std::get<std::string>(v_);
  }
  const Bytes& as_bytes() const {
    return std::get<Bytes>(v_);
  }
  const Array& as_array() const {
    return std::get<Array>(v_);
  }
  const Map& as_map() const {
    return std::get<Map>(v_);
  }
  EntityRef as_entity() const {
    return std::get<EntityRef>(v_);
  }
  FuncRef as_func() const {
    return std::get<FuncRef>(v_);
  }

  friend bool operator==(const Value& a, const Value& b) {
    return a.v_ == b.v_;
  }
  friend bool operator!=(const Value& a, const Value& b) {
    return !(a == b);
  }

 private:
  // Order MUST match the Tag enum so tag() == v_.index().
  std::variant<std::monostate, bool, std::int64_t, double, std::string, Bytes, Array, Map,
               EntityRef, FuncRef>
      v_;
};

}  // namespace kcd2s::sdk

#endif  // KCD2S_SDK_VALUE_HPP
