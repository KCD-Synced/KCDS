#include "kcd2s/sdk/idl.hpp"

#include <cctype>

namespace kcd2s::sdk::idl {

const char* side_name(Side s) {
  switch (s) {
    case Side::Shared:
      return "shared";
    case Side::Client:
      return "client";
    case Side::Server:
      return "server";
  }
  return "shared";
}
const char* stability_name(Stability s) {
  switch (s) {
    case Stability::Stable:
      return "stable";
    case Stability::Beta:
      return "beta";
    case Stability::Experimental:
      return "experimental";
  }
  return "stable";
}

namespace {

// Doc comment lines become one token each, marked by this first byte (never in an identifier).
constexpr char kDocMark = '\x01';

// Tokenizer: specials {}():,@ are single tokens; identifiers/numbers are [A-Za-z0-9_.]+; '#' and
// '//' start line comments; '///' lines are doc tokens (kDocMark + the line after "/// ").
std::vector<std::string> tokenize(const std::string& s) {
  std::vector<std::string> out;
  std::size_t i = 0;
  const std::size_t n = s.size();
  while (i < n) {
    char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '/' && s.compare(i, 3, "///") == 0) {
      std::size_t j = i + 3;
      if (j < n && s[j] == ' ')
        ++j;
      std::size_t end = s.find('\n', j);
      if (end == std::string::npos)
        end = n;
      std::string line = s.substr(j, end - j);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      out.push_back(kDocMark + line);
      i = end;
      continue;
    }
    if (c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) {
      while (i < n && s[i] != '\n')
        ++i;
      continue;
    }
    if (c == '{' || c == '}' || c == '(' || c == ')' || c == ':' || c == ',' || c == '@') {
      out.emplace_back(1, c);
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < n && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.'))
      ++j;
    if (j == i)
      return {};  // unexpected char
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

struct Parser {
  const std::vector<std::string>& t;
  std::size_t i = 0;
  std::string* err;

  bool fail(const std::string& m) {
    if (err)
      *err = m;
    return false;
  }
  const std::string& peek() const {
    static const std::string kEof;
    return i < t.size() ? t[i] : kEof;
  }
  bool eof() const {
    return i >= t.size();
  }
  std::string next() {
    return i < t.size() ? t[i++] : std::string();
  }
  bool accept(const std::string& tok) {
    if (peek() == tok) {
      ++i;
      return true;
    }
    return false;
  }
  bool expect(const std::string& tok) {
    if (!accept(tok))
      return fail("expected '" + tok + "' but got '" + peek() + "'");
    return true;
  }
  static bool is_ident(const std::string& s) {
    return !s.empty() && (std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_');
  }

  // The doc comment in front of the next declaration (empty when there is none).
  Doc take_doc() {
    std::vector<std::string> lines;
    while (!eof() && !t[i].empty() && t[i][0] == kDocMark)
      lines.push_back(t[i++].substr(1));
    return parse_doc(lines);
  }

  // Attributes after a declaration: @client @server @shared @stable @beta @experimental
  // @readonly @since(1.0)
  void apply_attr(const std::string& a, const std::string& arg, Side* side, Stability* stab,
                  std::string* since, bool* readonly) {
    if (a == "client" && side)
      *side = Side::Client;
    else if (a == "server" && side)
      *side = Side::Server;
    else if (a == "shared" && side)
      *side = Side::Shared;
    else if (a == "stable" && stab)
      *stab = Stability::Stable;
    else if (a == "beta" && stab)
      *stab = Stability::Beta;
    else if (a == "experimental" && stab)
      *stab = Stability::Experimental;
    else if (a == "readonly" && readonly)
      *readonly = true;
    else if (a == "since" && since)
      *since = arg;
  }

  bool parse_attrs(Side* side, Stability* stab, std::string* since, bool* readonly) {
    while (peek() == "@") {
      next();  // '@'
      const std::string a = next();
      if (!is_ident(a))
        return fail("expected attribute name after '@'");
      std::string arg;
      if (accept("(")) {
        arg = next();
        if (!expect(")"))
          return false;
      }
      apply_attr(a, arg, side, stab, since, readonly);
    }
    return true;
  }

  bool parse_params(std::vector<Param>& out) {
    if (!expect("("))
      return false;
    if (accept(")"))
      return true;
    while (true) {
      Param p;
      p.name = next();
      if (!is_ident(p.name))
        return fail("expected param name");
      if (!expect(":"))
        return false;
      p.type = next();
      if (!is_ident(p.type))
        return fail("expected param type");
      out.push_back(std::move(p));
      if (accept(","))
        continue;
      return expect(")");
    }
  }

  bool parse_type(TypeDef& td) {
    td.name = next();
    if (!is_ident(td.name))
      return fail("expected type name");
    if (accept(":")) {
      td.base = next();
      if (!is_ident(td.base))
        return fail("expected base type name");
    }
    if (!expect("{"))
      return false;
    while (true) {
      Doc doc = take_doc();
      if (accept("}"))
        break;
      if (eof())
        return fail("unexpected eof in type '" + td.name + "'");
      const std::string kw = next();
      if (kw == "prop") {
        Prop p;
        p.doc = std::move(doc);
        p.name = next();
        if (!expect(":"))
          return false;
        p.type = next();
        if (!parse_attrs(&p.side, &p.stability, &p.since, &p.readonly))
          return false;
        td.props.push_back(std::move(p));
      } else if (kw == "method") {
        Method m;
        m.doc = std::move(doc);
        m.name = next();
        if (!parse_params(m.params))
          return false;
        if (!expect(":"))
          return false;
        m.ret = next();
        if (!parse_attrs(&m.side, &m.stability, &m.since, nullptr))
          return false;
        td.methods.push_back(std::move(m));
      } else if (kw == "event") {
        Event e;
        e.doc = std::move(doc);
        e.name = next();
        if (!parse_params(e.params))
          return false;
        if (!parse_attrs(&e.side, nullptr, nullptr, nullptr))
          return false;
        td.events.push_back(std::move(e));
      } else {
        return fail("expected prop/method/event but got '" + kw + "'");
      }
    }
    return true;
  }

  bool parse(ApiModel& m) {
    while (!eof()) {
      Doc doc = take_doc();
      if (eof())
        break;
      const std::string kw = peek();
      if (kw == "api") {
        next();
        m.version = next();
      } else if (kw == "type") {
        next();
        TypeDef td;
        td.doc = std::move(doc);
        if (!parse_type(td))
          return false;
        m.types.push_back(std::move(td));
      } else {
        return fail("expected 'api' or 'type' but got '" + kw + "'");
      }
    }
    return true;
  }
};

std::string trim(const std::string& s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
    ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  return s.substr(b, e - b);
}

// "word rest" -> {word, rest}
std::pair<std::string, std::string> split_word(const std::string& s) {
  const std::string text = trim(s);
  const std::size_t space = text.find_first_of(" \t");
  if (space == std::string::npos)
    return {text, ""};
  return {text.substr(0, space), trim(text.substr(space))};
}

void append(std::string& to, const std::string& text) {
  if (text.empty())
    return;
  if (!to.empty())
    to += ' ';
  to += text;
}

}  // namespace

Doc parse_doc(const std::vector<std::string>& lines) {
  Doc doc;
  enum class In { Summary, Param, Throws, Example, Other } in = In::Summary;
  for (const std::string& raw : lines) {
    const std::string line = trim(raw);
    // a tag starts at the left edge; inside an example only a new tag ends the code
    if (!raw.empty() && raw[0] == '@') {
      auto [tag, rest] = split_word(line.substr(1));
      if (tag == "param") {
        auto [name, text] = split_word(rest);
        doc.params.emplace_back(name, text);
        in = In::Param;
      } else if (tag == "throws") {
        doc.throws.push_back(rest);
        in = In::Throws;
      } else if (tag == "raises") {
        std::string item;
        for (char c : rest + ",") {
          if (c == ',') {
            if (!trim(item).empty())
              doc.raises.push_back(trim(item));
            item.clear();
          } else {
            item += c;
          }
        }
        in = In::Other;
      } else if (tag == "group") {
        doc.group = rest;
        in = In::Other;
      } else if (tag == "kcds") {
        doc.kcds = true;
        in = In::Other;
      } else if (tag == "example") {
        Example example;
        auto [lang, side] = split_word(rest);
        if (!lang.empty())
          example.lang = lang;
        example.side = side;
        doc.examples.push_back(std::move(example));
        in = In::Example;
      } else {
        in = In::Other;  // an unknown tag is ignored, with its continuation lines
      }
      continue;
    }
    switch (in) {
      case In::Summary:
        append(doc.summary, line);
        break;
      case In::Param:
        append(doc.params.back().second, line);
        break;
      case In::Throws:
        append(doc.throws.back(), line);
        break;
      case In::Example: {
        std::string& code = doc.examples.back().code;
        if (!code.empty() || !line.empty())
          code += (code.empty() ? "" : "\n") + raw;
        break;
      }
      case In::Other:
        break;
    }
  }
  for (Example& example : doc.examples) {
    while (!example.code.empty() && std::isspace(static_cast<unsigned char>(example.code.back())))
      example.code.pop_back();
  }
  return doc;
}

std::optional<ApiModel> parse_idl(const std::string& text, std::string* err) {
  auto tokens = tokenize(text);
  ApiModel m;
  Parser p{tokens, 0, err};
  if (!p.parse(m))
    return std::nullopt;
  return m;
}

}  // namespace kcd2s::sdk::idl
