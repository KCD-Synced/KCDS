#include "kcd2s/sdk/nui.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "kcd2s/sdk/marshal.hpp"

namespace kcd2s::sdk {

namespace {

void json_escape(const std::string& s, std::string& out) {
  static constexpr char kPlain[] = "\"\\\n\r\t";
  static constexpr char kLetter[] = "\"\\nrt";
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (const char c : s) {
    const auto byte = static_cast<unsigned char>(c);
    int named = -1;
    for (int index = 0; index < 5 && named < 0; ++index) {
      if (kPlain[index] == c) {
        named = index;
      }
    }
    if (named >= 0) {
      out.push_back('\\');
      out.push_back(kLetter[named]);
    } else if (byte < 0x20) {
      out += "\\u00";
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 15]);
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
}

void emit(const Value& v, std::string& out) {
  switch (v.tag()) {
    case Tag::Null:
      out += "null";
      break;
    case Tag::Bool:
      out += v.as_bool() ? "true" : "false";
      break;
    case Tag::Int:
      out += std::to_string(v.as_int());
      break;
    case Tag::Double: {
      const double d = v.as_double();
      if (std::isfinite(d)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        out += buf;
        // Keep it a JSON float so json_to_value reads it back as a Double, not an Int (round-trip).
        if (!std::strpbrk(buf, ".eEnN"))
          out += ".0";
      } else {
        out += "null";  // JSON has no NaN/Inf
      }
      break;
    }
    case Tag::Str:
      json_escape(v.as_str(), out);
      break;
    case Tag::Bytes: {
      out += "{\"__bytes\":[";
      const auto& b = v.as_bytes();
      for (std::size_t i = 0; i < b.size(); ++i) {
        if (i)
          out.push_back(',');
        out += std::to_string(static_cast<unsigned>(b[i]));
      }
      out += "]}";
      break;
    }
    case Tag::Array: {
      out.push_back('[');
      const auto& a = v.as_array();
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (i)
          out.push_back(',');
        emit(a[i], out);
      }
      out.push_back(']');
      break;
    }
    case Tag::Map: {
      out.push_back('{');
      const auto& m = v.as_map();
      for (std::size_t i = 0; i < m.size(); ++i) {
        if (i)
          out.push_back(',');
        json_escape(m[i].first, out);
        out.push_back(':');
        emit(m[i].second, out);
      }
      out.push_back('}');
      break;
    }
    case Tag::EntityRef:
      out += "{\"__entity\":";
      out += std::to_string(v.as_entity().id);
      out.push_back('}');
      break;
    case Tag::FuncRef:
      out += "{\"__func\":[";
      out += std::to_string(v.as_func().resource);
      out.push_back(',');
      out += std::to_string(v.as_func().id);
      out += "]}";
      break;
  }
}

}  // namespace

std::string value_to_json(const Value& v) {
  std::string out;
  emit(v, out);
  return out;
}

namespace {

constexpr int kMaxJsonDepth = 64;

struct JsonParser {
  const char* p;
  const char* end;

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
      ++p;
  }

  bool lit(const char* s, std::size_t n) {
    if (static_cast<std::size_t>(end - p) < n || std::memcmp(p, s, n) != 0)
      return false;
    p += n;
    return true;
  }

  static bool hex4(const char* q, std::uint32_t& out) {
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = q[i];
      out <<= 4;
      if (c >= '0' && c <= '9')
        out |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        out |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        out |= static_cast<std::uint32_t>(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }

  static void utf8(std::uint32_t cp, std::string& s) {
    if (cp < 0x80) {
      s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool string(std::string& s) {
    if (p >= end || *p != '"')
      return false;
    ++p;
    while (p < end) {
      const char c = *p++;
      if (c == '"')
        return true;
      if (c == '\\') {
        if (p >= end)
          return false;
        const char e = *p++;
        switch (e) {
          case '"':
            s.push_back('"');
            break;
          case '\\':
            s.push_back('\\');
            break;
          case '/':
            s.push_back('/');
            break;
          case 'b':
            s.push_back('\b');
            break;
          case 'f':
            s.push_back('\f');
            break;
          case 'n':
            s.push_back('\n');
            break;
          case 'r':
            s.push_back('\r');
            break;
          case 't':
            s.push_back('\t');
            break;
          case 'u': {
            if (end - p < 4)
              return false;
            std::uint32_t cp = 0;
            if (!hex4(p, cp))
              return false;
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate -> need a low surrogate
              if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
                return false;
              std::uint32_t lo = 0;
              if (!hex4(p + 2, lo))
                return false;
              if (lo < 0xDC00 || lo > 0xDFFF)
                return false;
              p += 6;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              return false;  // lone low surrogate
            }
            utf8(cp, s);
            break;
          }
          default:
            return false;
        }
      } else if (static_cast<unsigned char>(c) < 0x20) {
        return false;  // raw control char not allowed in a JSON string
      } else {
        s.push_back(c);
      }
    }
    return false;  // unterminated
  }

  bool number(Value& out) {
    const char* start = p;
    if (p < end && *p == '-')
      ++p;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                       *p == '+' || *p == '-'))
      ++p;
    const std::size_t n = static_cast<std::size_t>(p - start);
    if (n == 0)
      return false;
    const bool is_float =
        std::memchr(start, '.', n) || std::memchr(start, 'e', n) || std::memchr(start, 'E', n);
    if (!is_float) {
      long long iv = 0;
      auto [ptr, ec] = std::from_chars(start, p, iv);
      if (ec == std::errc{} && ptr == p) {
        out = Value(static_cast<std::int64_t>(iv));
        return true;
      }
      // out of range -> fall through to double
    }
    double dv = 0.0;
    auto [ptr, ec] = std::from_chars(start, p, dv);
    if (ec != std::errc{} || ptr != p)
      return false;
    out = Value(dv);
    return true;
  }

  bool array(Value& out, int depth) {
    ++p;  // consume '['
    Value::Array a;
    ws();
    if (p < end && *p == ']') {
      ++p;
      out = Value(std::move(a));
      return true;
    }
    for (;;) {
      Value e;
      if (!value(e, depth))
        return false;
      a.push_back(std::move(e));
      ws();
      if (p >= end)
        return false;
      if (*p == ',') {
        ++p;
        ws();
        continue;
      }
      if (*p == ']') {
        ++p;
        out = Value(std::move(a));
        return true;
      }
      return false;
    }
  }

  // Recognise the reserved wrappers value_to_json emits so round-trips are identity.
  static bool as_wrapper(const Value::Map& m, Value& out) {
    if (m.size() != 1)
      return false;
    const std::string& k = m[0].first;
    const Value& v = m[0].second;
    if (k == "__entity" && v.tag() == Tag::Int) {
      out = Value(EntityRef{static_cast<core::EntityId>(v.as_int())});
      return true;
    }
    if (k == "__func" && v.tag() == Tag::Array && v.as_array().size() == 2 &&
        v.as_array()[0].tag() == Tag::Int && v.as_array()[1].tag() == Tag::Int) {
      out = Value(FuncRef{static_cast<std::uint32_t>(v.as_array()[0].as_int()),
                          static_cast<std::uint32_t>(v.as_array()[1].as_int())});
      return true;
    }
    if (k == "__bytes" && v.tag() == Tag::Array) {
      Value::Bytes b;
      b.reserve(v.as_array().size());
      for (const Value& e : v.as_array()) {
        if (e.tag() != Tag::Int || e.as_int() < 0 || e.as_int() > 255)
          return false;
        b.push_back(static_cast<std::uint8_t>(e.as_int()));
      }
      out = Value::bytes(std::move(b));
      return true;
    }
    return false;
  }

  bool object(Value& out, int depth) {
    ++p;  // consume '{'
    Value::Map m;
    ws();
    if (p < end && *p == '}') {
      ++p;
      out = Value(std::move(m));
      return true;
    }
    for (;;) {
      ws();
      std::string key;
      if (!string(key))
        return false;
      ws();
      if (p >= end || *p != ':')
        return false;
      ++p;
      ws();
      Value v;
      if (!value(v, depth))
        return false;
      m.emplace_back(std::move(key), std::move(v));
      ws();
      if (p >= end)
        return false;
      if (*p == ',') {
        ++p;
        continue;
      }
      if (*p == '}') {
        ++p;
        Value w;
        if (as_wrapper(m, w)) {
          out = std::move(w);
        } else {
          out = Value(std::move(m));
        }
        return true;
      }
      return false;
    }
  }

  bool value(Value& out, int depth) {
    if (depth > kMaxJsonDepth)
      return false;
    ws();
    if (p >= end)
      return false;
    switch (*p) {
      case 'n':
        if (!lit("null", 4))
          return false;
        out = Value::null();
        return true;
      case 't':
        if (!lit("true", 4))
          return false;
        out = Value(true);
        return true;
      case 'f':
        if (!lit("false", 5))
          return false;
        out = Value(false);
        return true;
      case '"': {
        std::string s;
        if (!string(s))
          return false;
        out = Value(std::move(s));
        return true;
      }
      case '[':
        return array(out, depth + 1);
      case '{':
        return object(out, depth + 1);
      default:
        return number(out);
    }
  }
};

}  // namespace

JsonResult json_to_value(const char* data, std::size_t len) {
  JsonResult r;
  JsonParser jp{data, data + len};
  jp.ws();
  Value v;
  if (!jp.value(v, 0))
    return r;
  jp.ws();
  if (jp.p != jp.end)
    return r;  // strict: no trailing garbage
  r.ok = true;
  r.value = std::move(v);
  return r;
}

JsonResult json_to_value(const std::string& text) {
  return json_to_value(text.data(), text.size());
}

NuiBus::CallResult NuiBus::invoke_callback_json(const std::string& resource,
                                                const std::string& name, const std::string& json) {
  CallResult r;
  JsonResult jr = json_to_value(json);
  if (!jr.ok)
    return r;
  auto out = invoke_callback(resource, name, jr.value);
  if (!out)
    return r;
  r.ok = true;
  r.result = std::move(*out);
  return r;
}

std::string NuiBus::key(const std::string& resource, const std::string& name) {
  std::string k = resource;
  k.push_back('\0');
  k += name;
  return k;
}

void NuiBus::register_ui(const std::string& resource, const std::string& page) {
  ui_pages_[resource] = page;
  if (renderer_)
    renderer_->CreateSurface(resource, page);
}

void NuiBus::unregister_ui(const std::string& resource) {
  if (ui_pages_.erase(resource) && renderer_)
    renderer_->DestroySurface(resource);
}

const std::string* NuiBus::ui_page(const std::string& resource) const {
  auto it = ui_pages_.find(resource);
  return it == ui_pages_.end() ? nullptr : &it->second;
}

void NuiBus::send_message(const std::string& resource, const Value& msg) {
  if (outgoing_)
    outgoing_(resource, msg);
  if (renderer_)
    renderer_->PostMessage(resource, value_to_json(msg));
}

void NuiBus::register_callback(const std::string& resource, const std::string& name,
                               CallbackFn fn) {
  callbacks_[key(resource, name)] = std::move(fn);
}

void NuiBus::clear_callbacks(const std::string& resource) {
  const std::string prefix = resource + '\0';
  for (auto it = callbacks_.begin(); it != callbacks_.end();) {
    // A key belongs to `resource` iff it starts with "resource\0".
    if (it->first.size() >= prefix.size() && it->first.compare(0, prefix.size(), prefix) == 0)
      it = callbacks_.erase(it);
    else
      ++it;
  }
}

std::optional<Value> NuiBus::invoke_callback(const std::string& resource, const std::string& name,
                                             const Value& body) {
  auto it = callbacks_.find(key(resource, name));
  if (it == callbacks_.end())
    return std::nullopt;
  return it->second(body);
}

NuiBus::CallResult NuiBus::invoke_callback_raw(const std::string& resource, const std::string& name,
                                               const std::uint8_t* data, std::size_t len) {
  CallResult r;
  DecodeResult dr = decode(data, len);  // untrusted bytes from the page
  if (!dr.ok)
    return r;
  auto out = invoke_callback(resource, name, dr.value);
  if (!out)
    return r;  // no such callback
  r.ok = true;
  r.result = std::move(*out);
  return r;
}

void NuiBus::set_focus(bool has_focus, bool has_cursor) {
  has_focus_ = has_focus;
  has_cursor_ = has_cursor;
  if (renderer_)
    renderer_->SetFocused(has_focus, has_cursor);
}

}  // namespace kcd2s::sdk
