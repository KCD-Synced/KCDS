#include "kcd2s/sdk/gen.hpp"

#include <sstream>

namespace kcd2s::sdk::gen {

using namespace kcd2s::sdk::idl;

namespace {

std::string ts_type(const std::string& t) {
  if (t == "void")
    return "void";
  if (t == "bool")
    return "boolean";
  if (t == "int" || t == "float")
    return "number";
  if (t == "string")
    return "string";
  if (t == "vec3")
    return "Vec3";
  if (t == "quat")
    return "Quat";
  if (t == "entity")
    return "EntityId";
  if (t == "any")
    return "any";
  return t;  // a declared type name
}

// "*/" inside a summary would end the JSDoc comment early.
std::string jsdoc_text(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    out += text[i];
    if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/')
      out += ' ';
  }
  return out;
}

std::string doc(Side side, Stability stab, const std::string& since, const std::string& summary) {
  std::ostringstream os;
  os << "  /** ";
  if (!summary.empty())
    os << jsdoc_text(summary) << " ";
  os << "@" << side_name(side) << " @" << stability_name(stab);
  if (!since.empty())
    os << " @since " << since;
  os << " */\n";
  return os.str();
}

std::string params_ts(const std::vector<Param>& ps) {
  std::ostringstream os;
  for (std::size_t i = 0; i < ps.size(); ++i) {
    if (i)
      os << ", ";
    os << ps[i].name << ": " << ts_type(ps[i].type);
  }
  return os.str();
}

// One TypeScript line per member, shared by the .d.ts and the docs JSON.
std::string prop_ts(const Prop& p) {
  return (p.readonly ? "readonly " : "") + p.name + ": " + ts_type(p.type);
}

std::string method_ts(const Method& m) {
  return m.name + "(" + params_ts(m.params) + "): " + ts_type(m.ret);
}

std::string event_ts(const Event& e) {
  return "on(event: \"" + e.name + "\", cb: (" + params_ts(e.params) + ") => void): void";
}

std::string lua_type(const std::string& t) {
  if (t == "void")
    return "nil";
  if (t == "bool")
    return "boolean";
  if (t == "int")
    return "integer";
  if (t == "float")
    return "number";
  if (t == "string")
    return "string";
  if (t == "vec3")
    return "Vec3";
  if (t == "quat")
    return "Quat";
  if (t == "entity")
    return "integer";  // EntityId
  if (t == "any")
    return "any";
  return t;
}

std::string cs_type(const std::string& t) {
  if (t == "void")
    return "void";
  if (t == "bool")
    return "bool";
  if (t == "int")
    return "long";
  if (t == "float")
    return "float";
  if (t == "string")
    return "string";
  if (t == "vec3")
    return "Vec3";
  if (t == "quat")
    return "Quat";
  if (t == "entity")
    return "EntityId";
  if (t == "any")
    return "object";
  return t;
}

}  // namespace

std::string emit_typescript(const ApiModel& model) {
  std::ostringstream os;
  os << "// Generated from Backend/sdk/api.idl by kcd2s Backend/sdk/gen — DO NOT EDIT. API v"
     << (model.version.empty() ? "0" : model.version) << ".\n\n";
  os << "export type EntityId = number;\n";
  os << "export interface Vec3 { x: number; y: number; z: number; }\n";
  os << "export interface Quat { x: number; y: number; z: number; w: number; }\n\n";

  for (const TypeDef& td : model.types) {
    if (!td.doc.summary.empty())
      os << "/** " << jsdoc_text(td.doc.summary) << " */\n";
    os << "export interface " << td.name;
    if (!td.base.empty())
      os << " extends " << td.base;
    os << " {\n";

    for (const Prop& p : td.props) {
      os << doc(p.side, p.stability, p.since, p.doc.summary);
      os << "  " << prop_ts(p) << ";\n";
    }
    for (const Method& m : td.methods) {
      os << doc(m.side, m.stability, m.since, m.doc.summary);
      os << "  " << method_ts(m) << ";\n";
    }
    for (const Event& e : td.events) {
      os << "  /** ";
      if (!e.doc.summary.empty())
        os << jsdoc_text(e.doc.summary) << " ";
      os << "@" << side_name(e.side) << " event */\n";
      os << "  " << event_ts(e) << ";\n";
    }
    os << "}\n\n";
  }
  return os.str();
}

namespace {

std::string json_string(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (u < 0x20) {
      static const char* hex = "0123456789abcdef";
      out += "\\u00";
      out += hex[u >> 4];
      out += hex[u & 15];
    } else {
      out += c;
    }
  }
  return out + "\"";
}

std::string json_strings(const std::vector<std::string>& items) {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i)
      out += ", ";
    out += json_string(items[i]);
  }
  return out + "]";
}

std::string param_description(const Doc& doc, const std::string& name) {
  for (const auto& [param, text] : doc.params) {
    if (param == name)
      return text;
  }
  return "";
}

std::string json_params(const std::vector<Param>& params, const Doc& doc) {
  std::string out = "[";
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i)
      out += ", ";
    out += "{\"name\": " + json_string(params[i].name);
    out += ", \"type\": " + json_string(ts_type(params[i].type));
    out += ", \"optional\": false, \"default\": null";
    out += ", \"description\": " + json_string(param_description(doc, params[i].name)) + "}";
  }
  return out + "]";
}

std::string json_examples(const std::vector<Example>& examples) {
  std::string out = "[";
  for (std::size_t i = 0; i < examples.size(); ++i) {
    if (i)
      out += ", ";
    out += "{\"lang\": " + json_string(examples[i].lang);
    out += ", \"side\": " + json_string(examples[i].side);
    out += ", \"code\": " + json_string(examples[i].code) + "}";
  }
  return out + "]";
}

// The fields every member has, after kind/name/signature.
std::string json_member_tail(const Doc& doc, Side side, const std::string& stability,
                             const std::string& since) {
  std::string out;
  out += ", \"side\": " + json_string(side_name(side));
  out += ", \"stability\": " + json_string(stability);
  out += ", \"since\": " + json_string(since);
  out += ", \"summary\": " + json_string(doc.summary);
  out += ", \"throws\": " + json_strings(doc.throws);
  out += ", \"raises\": " + json_strings(doc.raises);
  out += ", \"examples\": " + json_examples(doc.examples);
  out += std::string(", \"kcds\": ") + (doc.kcds ? "true" : "false");
  return out;
}

}  // namespace

std::string emit_docs_json(const ApiModel& model) {
  std::ostringstream os;
  os << "{\n  \"schema\": 1,\n  \"generator\": \"kcd2s-gen\",\n";
  os << "  \"apiVersion\": " << json_string(model.version) << ",\n  \"types\": [";
  for (std::size_t t = 0; t < model.types.size(); ++t) {
    const TypeDef& td = model.types[t];
    os << (t ? ",\n" : "\n") << "    {\"name\": " << json_string(td.name);
    os << ", \"base\": " << json_string(td.base);
    os << ", \"group\": " << json_string(td.doc.group);
    os << ", \"summary\": " << json_string(td.doc.summary) << ", \"since\": \"\",";
    os << "\n      \"members\": [";
    bool first = true;
    const auto open = [&]() {
      os << (first ? "\n" : ",\n") << "        ";
      first = false;
    };
    for (const Prop& p : td.props) {
      open();
      os << "{\"kind\": \"property\", \"name\": " << json_string(p.name);
      os << ", \"signature\": " << json_string(prop_ts(p));
      os << ", \"type\": " << json_string(ts_type(p.type));
      os << ", \"params\": [], \"returns\": \"\"";
      os << ", \"readonly\": " << (p.readonly ? "true" : "false");
      os << json_member_tail(p.doc, p.side, stability_name(p.stability), p.since) << "}";
    }
    for (const Method& m : td.methods) {
      open();
      os << "{\"kind\": \"method\", \"name\": " << json_string(m.name);
      os << ", \"signature\": " << json_string(method_ts(m));
      os << ", \"type\": \"\", \"params\": " << json_params(m.params, m.doc);
      os << ", \"returns\": " << json_string(ts_type(m.ret)) << ", \"readonly\": false";
      os << json_member_tail(m.doc, m.side, stability_name(m.stability), m.since) << "}";
    }
    for (const Event& e : td.events) {
      open();
      os << "{\"kind\": \"event\", \"name\": " << json_string(e.name);
      os << ", \"signature\": " << json_string(event_ts(e));
      os << ", \"type\": \"\", \"params\": " << json_params(e.params, e.doc);
      os << ", \"returns\": \"\", \"readonly\": false";
      os << json_member_tail(e.doc, e.side, "stable", "") << "}";
    }
    os << (first ? "]}" : "\n      ]}");
  }
  os << "\n  ]\n}\n";
  return os.str();
}

std::string emit_lua(const ApiModel& model) {
  std::ostringstream os;
  os << "---@meta\n";
  os << "-- Generated from Backend/sdk/api.idl by kcd2s Backend/sdk/gen — DO NOT EDIT. API v"
     << (model.version.empty() ? "0" : model.version) << ".\n";
  os << "---@class Vec3 @field x number @field y number @field z number\n";
  os << "---@class Quat @field x number @field y number @field z number @field w number\n\n";

  for (const TypeDef& td : model.types) {
    os << "---@class " << td.name;
    if (!td.base.empty())
      os << " : " << td.base;
    os << "\n";
    for (const Prop& p : td.props)
      os << "---@field " << p.name << " " << lua_type(p.type) << "\n";
    os << td.name << " = {}\n\n";

    for (const Method& m : td.methods) {
      for (const Param& pr : m.params)
        os << "---@param " << pr.name << " " << lua_type(pr.type) << "\n";
      if (m.ret != "void")
        os << "---@return " << lua_type(m.ret) << "\n";
      os << "function " << td.name << ":" << m.name << "(";
      for (std::size_t i = 0; i < m.params.size(); ++i) {
        if (i)
          os << ", ";
        os << m.params[i].name;
      }
      os << ") end\n\n";
    }
    for (const Event& e : td.events) {
      os << "--- event " << e.name << "(";
      for (std::size_t i = 0; i < e.params.size(); ++i) {
        if (i)
          os << ", ";
        os << e.params[i].name << ": " << lua_type(e.params[i].type);
      }
      os << ") [" << side_name(e.side) << "]\n";
    }
    if (!td.events.empty())
      os << "\n";
  }
  return os.str();
}

std::string emit_csharp(const ApiModel& model) {
  std::ostringstream os;
  os << "// Generated from Backend/sdk/api.idl by kcd2s Backend/sdk/gen — DO NOT EDIT. API v"
     << (model.version.empty() ? "0" : model.version) << ".\n";
  os << "using System;\n\nnamespace Kcd2Synced.Sdk {\n\n";
  os << "  public struct Vec3 { public float x, y, z; }\n";
  os << "  public struct Quat { public float x, y, z, w; }\n";
  os << "  public struct EntityId { public uint value; }\n\n";

  for (const TypeDef& td : model.types) {
    os << "  public partial class " << td.name;
    if (!td.base.empty())
      os << " : " << td.base;
    os << " {\n";
    for (const Prop& p : td.props) {
      os << "    /// <summary>@" << side_name(p.side) << " @" << stability_name(p.stability);
      if (!p.since.empty())
        os << " @since " << p.since;
      os << "</summary>\n";
      os << "    public " << cs_type(p.type) << " " << p.name << " { get;"
         << (p.readonly ? "" : " set;") << " }\n";
    }
    for (const Method& m : td.methods) {
      os << "    public " << cs_type(m.ret) << " " << m.name << "(";
      for (std::size_t i = 0; i < m.params.size(); ++i) {
        if (i)
          os << ", ";
        os << cs_type(m.params[i].type) << " " << m.params[i].name;
      }
      os << ");\n";
    }
    for (const Event& e : td.events) {
      os << "    public event Action";
      if (!e.params.empty()) {
        os << "<";
        for (std::size_t i = 0; i < e.params.size(); ++i) {
          if (i)
            os << ", ";
          os << cs_type(e.params[i].type);
        }
        os << ">";
      }
      os << " " << e.name << ";\n";
    }
    os << "  }\n\n";
  }
  os << "}\n";
  return os.str();
}

}  // namespace kcd2s::sdk::gen
