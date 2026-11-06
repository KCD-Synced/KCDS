// Small, strictly bounded JSON reader for launcher manifests, envelopes,
// deployment configuration and the install journal.
//
// This is deliberately not a general-purpose parser. It rejects anything the
// release contract does not need (comments, trailing data, control characters,
// numbers that are not finite, nesting past a fixed depth, documents past a
// caller-supplied byte budget) so a hostile or corrupt document fails closed
// instead of driving unbounded work on the player's machine.
#ifndef KCDS_LAUNCHER_JSON_HPP
#define KCDS_LAUNCHER_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kcds::launcher::json {

enum class Type { Null, Boolean, Number, String, Array, Object };

// Immutable parsed value. Objects preserve the first binding for a given key
// and reject duplicates during parsing, so lookups are unambiguous.
class Value {
 public:
  Value() = default;

  Type type() const {
    return type_;
  }
  bool is_object() const {
    return type_ == Type::Object;
  }
  bool is_array() const {
    return type_ == Type::Array;
  }
  bool is_string() const {
    return type_ == Type::String;
  }
  bool is_number() const {
    return type_ == Type::Number;
  }
  bool is_boolean() const {
    return type_ == Type::Boolean;
  }
  bool is_null() const {
    return type_ == Type::Null;
  }

  bool boolean() const {
    return boolean_;
  }
  double number() const {
    return number_;
  }

  // True only when the number was written without a fraction or exponent and
  // fits a signed 64-bit integer; mirrors Number.isSafeInteger expectations on
  // the publisher side. The integer value is returned through out.
  bool as_integer(std::int64_t& out) const;

  const std::string& string() const {
    return string_;
  }

  // Array element access; empty Value if out of range.
  std::size_t size() const {
    return array_.size();
  }
  const Value& at(std::size_t index) const;

  // Object member access. has() distinguishes an absent key from a present
  // null. member() returns a null Value when the key is absent.
  bool has(const std::string& key) const;
  const Value& member(const std::string& key) const;

  // Object members in document order (empty for any other type), so a strict
  // reader can reject keys it does not recognise or walk a map of entries.
  const std::vector<std::pair<std::string, Value>>& members() const {
    return object_;
  }

  // Builders used by the parser only.
  static Value make_null();
  static Value make_boolean(bool value);
  static Value make_number(double value, bool is_integer, std::int64_t integer);
  static Value make_string(std::string value);
  static Value make_array(std::vector<Value> items);
  static Value make_object(std::vector<std::pair<std::string, Value>> members);

 private:
  Type type_ = Type::Null;
  bool boolean_ = false;
  bool integer_ok_ = false;
  double number_ = 0.0;
  std::int64_t integer_ = 0;
  std::string string_;
  std::vector<Value> array_;
  std::vector<std::pair<std::string, Value>> object_;
};

struct Limits {
  std::size_t max_bytes = 256 * 1024;
  std::size_t max_depth = 32;
};

// Parses a complete JSON document. Returns false and sets error on any failure;
// out is only meaningful when the call returns true.
bool parse(const std::string& text, const Limits& limits, Value& out, std::string& error);

}  // namespace kcds::launcher::json

#endif  // KCDS_LAUNCHER_JSON_HPP
