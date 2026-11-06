#include "kcd2s/scripting/resource.hpp"

#include <algorithm>

namespace kcd2s::scripting {

namespace {

constexpr std::size_t kMaxTokens = 100000;  // bound untrusted manifests

std::optional<ScriptLang> ext_lang(const std::string& path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return std::nullopt;
  std::string e = path.substr(dot + 1);
  for (char& c : e)
    c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  if (e == "js")
    return ScriptLang::Js;
  if (e == "ts")
    return ScriptLang::Ts;
  if (e == "lua")
    return ScriptLang::Lua;
  if (e == "cs")
    return ScriptLang::CSharp;
  return std::nullopt;
}

// A directive: a key word followed by zero or more quoted-string operands.
struct Directive {
  std::string key;
  std::vector<std::string> values;
};

// Tokenize + group into directives. Returns false (with `err`) on a lexical error.
bool tokenize(const std::string& text, std::vector<Directive>& out, std::string& err) {
  const std::size_t n = text.size();
  std::size_t i = 0;
  std::size_t token_count = 0;

  // Pending directive being built.
  Directive cur;
  bool have_cur = false;
  bool in_block = false;  // inside a { ... } operand block

  auto flush_cur = [&]() {
    if (have_cur) {
      out.push_back(std::move(cur));
      cur = Directive{};
      have_cur = false;
    }
  };

  while (i < n) {
    const char c = text[i];
    // Whitespace.
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++i;
      continue;
    }
    // Comments: # ... or -- ... to end of line.
    if (c == '#' || (c == '-' && i + 1 < n && text[i + 1] == '-')) {
      while (i < n && text[i] != '\n')
        ++i;
      continue;
    }
    if (++token_count > kMaxTokens) {
      err = "manifest too large";
      return false;
    }
    if (c == '{') {
      if (!have_cur || in_block) {
        err = "unexpected '{'";
        return false;
      }
      in_block = true;
      ++i;
      continue;
    }
    if (c == '}') {
      if (!in_block) {
        err = "unexpected '}'";
        return false;
      }
      in_block = false;
      flush_cur();  // block closes the directive
      ++i;
      continue;
    }
    if (c == '"') {
      // Quoted string operand.
      ++i;
      std::string s;
      bool closed = false;
      while (i < n) {
        const char q = text[i++];
        if (q == '\\' && i < n) {  // simple escape
          s.push_back(text[i++]);
          continue;
        }
        if (q == '"') {
          closed = true;
          break;
        }
        s.push_back(q);
      }
      if (!closed) {
        err = "unterminated string";
        return false;
      }
      if (!have_cur) {
        err = "value with no key";
        return false;
      }
      cur.values.push_back(std::move(s));
      continue;
    }
    // Bareword = a key. Starting a new key flushes the previous directive (unless mid-block).
    {
      std::string w;
      while (i < n) {
        const char k = text[i];
        if (k == ' ' || k == '\t' || k == '\r' || k == '\n' || k == '{' || k == '}' || k == '"')
          break;
        w.push_back(k);
        ++i;
      }
      if (in_block) {
        err = "expected string inside block, got '" + w + "'";
        return false;
      }
      flush_cur();
      cur.key = std::move(w);
      have_cur = true;
    }
  }
  if (in_block) {
    err = "unterminated block";
    return false;
  }
  flush_cur();
  return true;
}

bool add_scripts(ResourceManifest& m, const std::vector<std::string>& paths, ScriptSide side,
                 std::string& err) {
  for (const std::string& p : paths) {
    auto lang = ext_lang(p);
    if (!lang) {
      err = "unknown script language for '" + p + "'";
      return false;
    }
    m.scripts.push_back(ScriptDecl{p, *lang, side});
  }
  return true;
}

}  // namespace

std::optional<ScriptLang> lang_of_path(const std::string& path) {
  return ext_lang(path);
}

const char* lang_name(ScriptLang l) {
  switch (l) {
    case ScriptLang::Js:
      return "js";
    case ScriptLang::Ts:
      return "ts";
    case ScriptLang::Lua:
      return "lua";
    case ScriptLang::CSharp:
      return "cs";
  }
  return "?";
}

std::vector<const ScriptDecl*> ResourceManifest::by_side(ScriptSide side) const {
  std::vector<const ScriptDecl*> out;
  for (const ScriptDecl& s : scripts)
    if (s.side == side)
      out.push_back(&s);
  return out;
}

ParseResult parse_manifest(const std::string& text) {
  ParseResult r;
  std::vector<Directive> directives;
  if (!tokenize(text, directives, r.error))
    return r;

  ResourceManifest& m = r.manifest;
  auto single = [](const Directive& d) -> const std::string& {
    static const std::string empty;
    return d.values.empty() ? empty : d.values.front();
  };

  for (const Directive& d : directives) {
    if (d.key == "name") {
      m.name = single(d);
    } else if (d.key == "version") {
      m.version = single(d);
    } else if (d.key == "author") {
      m.author = single(d);
    } else if (d.key == "description") {
      m.description = single(d);
    } else if (d.key == "client_script" || d.key == "client_scripts") {
      if (!add_scripts(m, d.values, ScriptSide::Client, r.error))
        return r;
    } else if (d.key == "server_script" || d.key == "server_scripts") {
      if (!add_scripts(m, d.values, ScriptSide::Server, r.error))
        return r;
    } else if (d.key == "shared_script" || d.key == "shared_scripts") {
      if (!add_scripts(m, d.values, ScriptSide::Shared, r.error))
        return r;
    } else if (d.key == "file" || d.key == "files") {
      m.files.insert(m.files.end(), d.values.begin(), d.values.end());
    } else if (d.key == "dependency" || d.key == "dependencies") {
      m.dependencies.insert(m.dependencies.end(), d.values.begin(), d.values.end());
    } else if (d.key == "export" || d.key == "exports") {
      m.exports.insert(m.exports.end(), d.values.begin(), d.values.end());
    } else if (d.key == "ui_page") {
      m.ui_page = single(d);
    }
    // Unknown keys: ignored (forward-compatible).
  }

  if (m.name.empty()) {
    r.error = "manifest missing required 'name'";
    return r;
  }
  if (m.version.empty()) {
    r.error = "manifest missing required 'version'";
    return r;
  }
  r.ok = true;
  return r;
}

bool ResourceRegistry::add(ResourceManifest m, std::string& err) {
  if (find(m.name) != nullptr) {
    err = "duplicate resource '" + m.name + "'";
    return false;
  }
  resources_.push_back(std::move(m));
  return true;
}

const ResourceManifest* ResourceRegistry::find(const std::string& name) const {
  for (const ResourceManifest& m : resources_)
    if (m.name == name)
      return &m;
  return nullptr;
}

bool ResourceRegistry::resolve_start_order(std::vector<std::string>& out_order,
                                           std::string& err) const {
  out_order.clear();

  // Deterministic: visit resource names in sorted order; DFS post-order yields dependencies first.
  std::vector<std::string> names;
  names.reserve(resources_.size());
  for (const ResourceManifest& m : resources_)
    names.push_back(m.name);
  std::sort(names.begin(), names.end());

  enum Color { White, Gray, Black };
  std::vector<std::pair<std::string, Color>> color;
  auto color_of = [&](const std::string& nm) -> Color* {
    for (auto& [k, c] : color)
      if (k == nm)
        return &c;
    color.push_back({nm, White});
    return &color.back().second;
  };

  // Iterative DFS with an explicit stack so a deep/cyclic graph cannot overflow the call stack.
  struct Frame {
    std::string node;
    std::size_t next_dep;
  };

  for (const std::string& root : names) {
    if (*color_of(root) != White)
      continue;
    std::vector<Frame> stack;
    stack.push_back({root, 0});
    *color_of(root) = Gray;

    while (!stack.empty()) {
      Frame& f = stack.back();
      const ResourceManifest* m = find(f.node);
      // m is always non-null here (roots come from resources_, deps are validated below).

      // Dependencies visited in sorted order for determinism.
      std::vector<std::string> deps = m->dependencies;
      std::sort(deps.begin(), deps.end());

      if (f.next_dep < deps.size()) {
        const std::string& dep = deps[f.next_dep++];
        if (find(dep) == nullptr) {
          err = "resource '" + f.node + "' depends on unknown resource '" + dep + "'";
          return false;
        }
        Color* dc = color_of(dep);
        if (*dc == Gray) {
          err = "dependency cycle involving '" + dep + "'";
          return false;
        }
        if (*dc == White) {
          *dc = Gray;
          stack.push_back({dep, 0});
        }
        continue;
      }

      // All deps done -> emit this node.
      *color_of(f.node) = Black;
      out_order.push_back(f.node);
      stack.pop_back();
    }
  }
  return true;
}

}  // namespace kcd2s::scripting
