#include "kcd2s/platform/server_browser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace kcd2s::platform {

namespace {

void append_utf8(std::string& out, unsigned code) {
  if (code < 0x80) {
    out.push_back(char(code));
  } else if (code < 0x800) {
    out.push_back(char(0xC0 | (code >> 6)));
    out.push_back(char(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out.push_back(char(0xE0 | (code >> 12)));
    out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(char(0x80 | (code & 0x3F)));
  } else {
    out.push_back(char(0xF0 | (code >> 18)));
    out.push_back(char(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(char(0x80 | (code & 0x3F)));
  }
}

// Minimal JSON reader for the /servers shape: an array of flat objects whose values are strings,
// numbers, bools, null, or a string array (tags). Not a general JSON parser.
class Reader {
 public:
  explicit Reader(const std::string& s) : p_(s.data()), end_(s.data() + s.size()) {
  }

  bool parse_servers(std::vector<ServerEntry>& out) {
    ws();
    if (!eat('['))
      return false;
    ws();
    if (peek() == ']') {
      ++p_;
      return true;
    }
    while (true) {
      ServerEntry e;
      if (!parse_entry(e))
        return false;
      out.push_back(std::move(e));
      ws();
      if (peek() == ',') {
        ++p_;
        ws();
        continue;
      }
      if (peek() == ']') {
        ++p_;
        return true;
      }
      return false;
    }
  }

 private:
  const char* p_;
  const char* end_;

  char peek() const {
    return p_ < end_ ? *p_ : '\0';
  }
  void ws() {
    while (p_ < end_ && std::isspace(static_cast<unsigned char>(*p_)))
      ++p_;
  }
  bool eat(char c) {
    if (peek() != c)
      return false;
    ++p_;
    return true;
  }

  bool parse_hex4(unsigned& value) {
    if (end_ - p_ < 4)
      return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = *p_++;
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= unsigned(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= unsigned(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= unsigned(c - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  }

  // Decodes the digits of a unicode escape (the master escapes <, > and &) to UTF-8 and joins a
  // surrogate pair. A lone surrogate becomes U+FFFD.
  bool parse_unicode_escape(std::string& out) {
    unsigned code = 0;
    if (!parse_hex4(code))
      return false;
    const bool high = code >= 0xD800 && code <= 0xDBFF;
    if (high && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
      const char* rewind = p_;
      p_ += 2;
      unsigned low = 0;
      if (parse_hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
      } else {
        p_ = rewind;
      }
    }
    if (code >= 0xD800 && code <= 0xDFFF)
      code = 0xFFFD;
    append_utf8(out, code);
    return true;
  }

  bool parse_string(std::string& out) {
    if (!eat('"'))
      return false;
    out.clear();
    while (p_ < end_) {
      char c = *p_++;
      if (c == '"')
        return true;
      if (c == '\\') {
        if (p_ >= end_)
          return false;
        char e = *p_++;
        switch (e) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'u':
            if (!parse_unicode_escape(out))
              return false;
            break;
          default:
            return false;
        }
      } else {
        out.push_back(c);
      }
    }
    return false;
  }

  bool parse_string_array(std::vector<std::string>& out) {
    if (!eat('['))
      return false;
    ws();
    if (peek() == ']') {
      ++p_;
      return true;
    }
    while (true) {
      ws();
      std::string s;
      if (!parse_string(s))
        return false;
      out.push_back(std::move(s));
      ws();
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == ']') {
        ++p_;
        return true;
      }
      return false;
    }
  }

  std::string parse_bare() {
    std::string s;
    while (p_ < end_) {
      char c = *p_;
      if (c == ',' || c == '}' || c == ']' || std::isspace(static_cast<unsigned char>(c)))
        break;
      s.push_back(c);
      ++p_;
    }
    return s;
  }

  bool parse_entry(ServerEntry& e) {
    ws();
    if (!eat('{'))
      return false;
    ws();
    if (peek() == '}') {
      ++p_;
      return true;
    }
    while (true) {
      ws();
      std::string key;
      if (!parse_string(key))
        return false;
      ws();
      if (!eat(':'))
        return false;
      ws();

      if (key == "tags") {
        if (!parse_string_array(e.tags))
          return false;
      } else if (peek() == '"') {
        std::string v;
        if (!parse_string(v))
          return false;
        if (key == "id")
          e.id = v;
        else if (key == "name")
          e.name = v;
        else if (key == "address")
          e.address = v;
        else if (key == "gamemode")
          e.gamemode = v;
        else if (key == "map")
          e.map = v;
        else if (key == "version")
          e.version = v;
        else if (key == "lastSeen")
          e.lastSeen = v;
        else if (key == "image")
          e.image = v;
      } else {
        const std::string v = parse_bare();
        if (key == "players")
          e.players = std::atoi(v.c_str());
        else if (key == "maxPlayers")
          e.maxPlayers = std::atoi(v.c_str());
        else if (key == "protocol")
          e.protocol = std::atoi(v.c_str());
        else if (key == "queue")
          e.queue = v == "true";
        else if (key == "password")
          e.password = v == "true";
      }

      ws();
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == '}') {
        ++p_;
        return true;
      }
      return false;
    }
  }
};

void append_string_field(std::string& out, const char* key, const std::string& value) {
  out += '"';
  out += key;
  out += "\":\"";
  append_json_string(out, value);
  out += '"';
}

// The fields a heartbeat and a list entry share, without the closing brace.
void append_entry_fields(std::string& out, const ServerEntry& e) {
  out += '{';
  append_string_field(out, "id", e.id);
  out += ',';
  append_string_field(out, "name", e.name);
  out += ',';
  append_string_field(out, "address", e.address);
  out += ',';
  append_string_field(out, "gamemode", e.gamemode);
  out += ',';
  append_string_field(out, "map", e.map);
  out += ',';
  append_string_field(out, "version", e.version);
  out += ",\"players\":" + std::to_string(e.players);
  out += ",\"maxPlayers\":" + std::to_string(e.maxPlayers);
  out += ",\"protocol\":" + std::to_string(e.protocol);
  out += ',';
  append_string_field(out, "image", e.image);
  out += ",\"queue\":";
  out += e.queue ? "true" : "false";
  // Only when set: a Master from before refuses a heartbeat with a field it does not
  // know.
  if (e.password) {
    out += ",\"password\":true";
  }
  out += ",\"tags\":[";
  for (std::size_t i = 0; i < e.tags.size(); ++i) {
    if (i)
      out += ',';
    out += '"';
    append_json_string(out, e.tags[i]);
    out += '"';
  }
  out += ']';
}

}  // namespace

std::optional<std::vector<ServerEntry>> parse_server_list(const std::string& json) {
  std::vector<ServerEntry> out;
  if (!Reader(json).parse_servers(out))
    return std::nullopt;
  return out;
}

void append_json_string(std::string& out, const std::string& s) {
  static constexpr char kPlain[] = "\"\\\n\t\r";
  static constexpr char kLetter[] = "\"\\ntr";
  static constexpr char kHex[] = "0123456789abcdef";
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
}

std::string encode_heartbeat(const ServerEntry& e) {
  std::string out;
  append_entry_fields(out, e);
  out += '}';
  return out;
}

std::string encode_server_list(const std::vector<ServerEntry>& servers) {
  std::string out = "[";
  for (std::size_t i = 0; i < servers.size(); ++i) {
    if (i)
      out += ',';
    append_entry_fields(out, servers[i]);
    out += ',';
    append_string_field(out, "lastSeen", servers[i].lastSeen);
    out += '}';
  }
  out += ']';
  return out;
}

std::vector<ServerEntry> ServerBrowser::view() const {
  std::vector<ServerEntry> out;
  for (const ServerEntry& s : servers_) {
    if (protocol_ != 0 && s.protocol != protocol_)
      continue;
    if (hide_full_ && s.maxPlayers > 0 && s.players >= s.maxPlayers)
      continue;
    if (!tag_.empty() && std::find(s.tags.begin(), s.tags.end(), tag_) == s.tags.end())
      continue;
    if (!text_.empty()) {
      const bool in_name = s.name.find(text_) != std::string::npos;
      const bool in_mode = s.gamemode.find(text_) != std::string::npos;
      if (!in_name && !in_mode)
        continue;
    }
    out.push_back(s);
  }

  // Stable sort ascending by the key, then reverse for descending — keeps ties in input order.
  std::stable_sort(out.begin(), out.end(), [this](const ServerEntry& a, const ServerEntry& b) {
    switch (sort_) {
      case Sort::Name:
        return a.name < b.name;
      case Sort::Players:
      default:
        return a.players < b.players;
    }
  });
  if (descending_)
    std::reverse(out.begin(), out.end());
  return out;
}

}  // namespace kcd2s::platform
