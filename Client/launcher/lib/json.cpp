#include "kcds/launcher/json.hpp"

#include <cmath>

namespace kcds::launcher::json {

namespace {

const Value& null_value() {
  static const Value value;
  return value;
}

// Recursive-descent reader over a fixed buffer. Every step is bounded: the
// cursor never passes end_, and depth is checked before each container.
class Reader {
 public:
  Reader(const std::string& text, const Limits& limits)
      : cursor_(text.data()), end_(text.data() + text.size()), limits_(limits) {
  }

  bool parse(Value& out, std::string& error) {
    skip_whitespace();
    if (!read_value(0, out)) {
      error = error_;
      return false;
    }
    skip_whitespace();
    if (cursor_ != end_) {
      error = "trailing data after JSON document";
      return false;
    }
    return true;
  }

 private:
  bool fail(const char* message) {
    if (error_.empty()) {
      error_ = message;
    }
    return false;
  }

  void skip_whitespace() {
    while (cursor_ != end_) {
      const char c = *cursor_;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++cursor_;
      } else {
        break;
      }
    }
  }

  bool read_value(std::size_t depth, Value& out) {
    if (cursor_ == end_) {
      return fail("unexpected end of JSON document");
    }
    const char c = *cursor_;
    switch (c) {
      case '{':
        return read_object(depth, out);
      case '[':
        return read_array(depth, out);
      case '"': {
        std::string text;
        if (!read_string(text)) {
          return false;
        }
        out = Value::make_string(std::move(text));
        return true;
      }
      case 't':
      case 'f':
        return read_boolean(out);
      case 'n':
        return read_null(out);
      default:
        if (c == '-' || (c >= '0' && c <= '9')) {
          return read_number(out);
        }
        return fail("unexpected character in JSON document");
    }
  }

  bool read_object(std::size_t depth, Value& out) {
    if (depth >= limits_.max_depth) {
      return fail("JSON nesting too deep");
    }
    ++cursor_;  // consume '{'
    std::vector<std::pair<std::string, Value>> members;
    skip_whitespace();
    if (cursor_ != end_ && *cursor_ == '}') {
      ++cursor_;
      out = Value::make_object(std::move(members));
      return true;
    }
    while (true) {
      skip_whitespace();
      if (cursor_ == end_ || *cursor_ != '"') {
        return fail("expected object key");
      }
      std::string key;
      if (!read_string(key)) {
        return false;
      }
      for (const auto& existing : members) {
        if (existing.first == key) {
          return fail("duplicate object key");
        }
      }
      skip_whitespace();
      if (cursor_ == end_ || *cursor_ != ':') {
        return fail("expected ':' after object key");
      }
      ++cursor_;
      skip_whitespace();
      Value child;
      if (!read_value(depth + 1, child)) {
        return false;
      }
      members.emplace_back(std::move(key), std::move(child));
      skip_whitespace();
      if (cursor_ == end_) {
        return fail("unterminated object");
      }
      if (*cursor_ == ',') {
        ++cursor_;
        continue;
      }
      if (*cursor_ == '}') {
        ++cursor_;
        out = Value::make_object(std::move(members));
        return true;
      }
      return fail("expected ',' or '}' in object");
    }
  }

  bool read_array(std::size_t depth, Value& out) {
    if (depth >= limits_.max_depth) {
      return fail("JSON nesting too deep");
    }
    ++cursor_;  // consume '['
    std::vector<Value> items;
    skip_whitespace();
    if (cursor_ != end_ && *cursor_ == ']') {
      ++cursor_;
      out = Value::make_array(std::move(items));
      return true;
    }
    while (true) {
      skip_whitespace();
      Value child;
      if (!read_value(depth + 1, child)) {
        return false;
      }
      items.push_back(std::move(child));
      skip_whitespace();
      if (cursor_ == end_) {
        return fail("unterminated array");
      }
      if (*cursor_ == ',') {
        ++cursor_;
        continue;
      }
      if (*cursor_ == ']') {
        ++cursor_;
        out = Value::make_array(std::move(items));
        return true;
      }
      return fail("expected ',' or ']' in array");
    }
  }

  bool read_string(std::string& out) {
    ++cursor_;  // consume opening quote
    out.clear();
    while (cursor_ != end_) {
      const unsigned char c = static_cast<unsigned char>(*cursor_);
      if (c == '"') {
        ++cursor_;
        return true;
      }
      if (c == '\\') {
        ++cursor_;
        if (!read_escape(out)) {
          return false;
        }
        continue;
      }
      if (c < 0x20) {
        return fail("control character in JSON string");
      }
      out.push_back(static_cast<char>(c));
      ++cursor_;
    }
    return fail("unterminated string");
  }

  bool read_escape(std::string& out) {
    if (cursor_ == end_) {
      return fail("unterminated escape");
    }
    const char c = *cursor_;
    ++cursor_;
    switch (c) {
      case '"':
        out.push_back('"');
        return true;
      case '\\':
        out.push_back('\\');
        return true;
      case '/':
        out.push_back('/');
        return true;
      case 'b':
        out.push_back('\b');
        return true;
      case 'f':
        out.push_back('\f');
        return true;
      case 'n':
        out.push_back('\n');
        return true;
      case 'r':
        out.push_back('\r');
        return true;
      case 't':
        out.push_back('\t');
        return true;
      case 'u':
        return read_unicode_escape(out);
      default:
        return fail("invalid escape in JSON string");
    }
  }

  bool read_hex4(std::uint32_t& out) {
    if (end_ - cursor_ < 4) {
      return fail("truncated \\u escape");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = *cursor_;
      ++cursor_;
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return fail("invalid hex digit in \\u escape");
      }
    }
    out = value;
    return true;
  }

  bool read_unicode_escape(std::string& out) {
    std::uint32_t code = 0;
    if (!read_hex4(code)) {
      return false;
    }
    if (code >= 0xD800 && code <= 0xDBFF) {
      // High surrogate: must be followed by \uDC00..\uDFFF.
      if (end_ - cursor_ < 2 || cursor_[0] != '\\' || cursor_[1] != 'u') {
        return fail("unpaired UTF-16 surrogate");
      }
      cursor_ += 2;
      std::uint32_t low = 0;
      if (!read_hex4(low)) {
        return false;
      }
      if (low < 0xDC00 || low > 0xDFFF) {
        return fail("invalid low surrogate");
      }
      code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
    } else if (code >= 0xDC00 && code <= 0xDFFF) {
      return fail("unexpected low surrogate");
    }
    encode_utf8(code, out);
    return true;
  }

  static void encode_utf8(std::uint32_t code, std::string& out) {
    if (code <= 0x7F) {
      out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  bool read_boolean(Value& out) {
    if (match("true")) {
      out = Value::make_boolean(true);
      return true;
    }
    if (match("false")) {
      out = Value::make_boolean(false);
      return true;
    }
    return fail("invalid literal");
  }

  bool read_null(Value& out) {
    if (match("null")) {
      out = Value::make_null();
      return true;
    }
    return fail("invalid literal");
  }

  bool match(const char* literal) {
    const char* p = cursor_;
    while (*literal) {
      if (p == end_ || *p != *literal) {
        return false;
      }
      ++p;
      ++literal;
    }
    cursor_ = p;
    return true;
  }

  bool read_number(Value& out) {
    const char* start = cursor_;
    bool is_integer = true;
    if (cursor_ != end_ && *cursor_ == '-') {
      ++cursor_;
    }
    if (cursor_ == end_ || *cursor_ < '0' || *cursor_ > '9') {
      return fail("invalid number");
    }
    if (*cursor_ == '0') {
      ++cursor_;
    } else {
      while (cursor_ != end_ && *cursor_ >= '0' && *cursor_ <= '9') {
        ++cursor_;
      }
    }
    if (cursor_ != end_ && *cursor_ == '.') {
      is_integer = false;
      ++cursor_;
      if (cursor_ == end_ || *cursor_ < '0' || *cursor_ > '9') {
        return fail("invalid number fraction");
      }
      while (cursor_ != end_ && *cursor_ >= '0' && *cursor_ <= '9') {
        ++cursor_;
      }
    }
    if (cursor_ != end_ && (*cursor_ == 'e' || *cursor_ == 'E')) {
      is_integer = false;
      ++cursor_;
      if (cursor_ != end_ && (*cursor_ == '+' || *cursor_ == '-')) {
        ++cursor_;
      }
      if (cursor_ == end_ || *cursor_ < '0' || *cursor_ > '9') {
        return fail("invalid number exponent");
      }
      while (cursor_ != end_ && *cursor_ >= '0' && *cursor_ <= '9') {
        ++cursor_;
      }
    }
    const std::string text(start, static_cast<std::size_t>(cursor_ - start));
    double value = 0.0;
    try {
      value = std::stod(text);
    } catch (...) {
      return fail("number out of range");
    }
    if (!std::isfinite(value)) {
      return fail("non-finite number");
    }
    std::int64_t integer = 0;
    bool integer_ok = false;
    if (is_integer) {
      integer_ok = parse_integer(text, integer);
    }
    out = Value::make_number(value, integer_ok, integer);
    return true;
  }

  static bool parse_integer(const std::string& text, std::int64_t& out) {
    try {
      std::size_t consumed = 0;
      const long long value = std::stoll(text, &consumed);
      if (consumed != text.size()) {
        return false;
      }
      out = static_cast<std::int64_t>(value);
      return true;
    } catch (...) {
      return false;
    }
  }

  const char* cursor_;
  const char* end_;
  Limits limits_;
  std::string error_;
};

}  // namespace

bool Value::as_integer(std::int64_t& out) const {
  if (type_ != Type::Number || !integer_ok_) {
    return false;
  }
  out = integer_;
  return true;
}

const Value& Value::at(std::size_t index) const {
  if (index >= array_.size()) {
    return null_value();
  }
  return array_[index];
}

bool Value::has(const std::string& key) const {
  for (const auto& member : object_) {
    if (member.first == key) {
      return true;
    }
  }
  return false;
}

const Value& Value::member(const std::string& key) const {
  for (const auto& member : object_) {
    if (member.first == key) {
      return member.second;
    }
  }
  return null_value();
}

Value Value::make_null() {
  Value value;
  value.type_ = Type::Null;
  return value;
}

Value Value::make_boolean(bool boolean) {
  Value value;
  value.type_ = Type::Boolean;
  value.boolean_ = boolean;
  return value;
}

Value Value::make_number(double number, bool is_integer, std::int64_t integer) {
  Value value;
  value.type_ = Type::Number;
  value.number_ = number;
  value.integer_ok_ = is_integer;
  value.integer_ = integer;
  return value;
}

Value Value::make_string(std::string text) {
  Value value;
  value.type_ = Type::String;
  value.string_ = std::move(text);
  return value;
}

Value Value::make_array(std::vector<Value> items) {
  Value value;
  value.type_ = Type::Array;
  value.array_ = std::move(items);
  return value;
}

Value Value::make_object(std::vector<std::pair<std::string, Value>> members) {
  Value value;
  value.type_ = Type::Object;
  value.object_ = std::move(members);
  return value;
}

bool parse(const std::string& text, const Limits& limits, Value& out, std::string& error) {
  if (text.size() > limits.max_bytes) {
    error = "JSON document exceeds byte budget";
    return false;
  }
  Reader reader(text, limits);
  return reader.parse(out, error);
}

}  // namespace kcds::launcher::json
