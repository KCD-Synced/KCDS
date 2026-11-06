// The canonical API IDL: one source of truth for every SDK object,
// method, property and event, with side (client/server/shared), stability and since-version. The
// generators (gen.hpp) emit TypeScript/.d.ts, Lua EmmyLua and C# from this one model, so the API
// cannot drift between languages. This header defines the model + a parser for the small textual
// IDL in Backend/sdk/api.idl.
#ifndef KCD2S_SDK_IDL_HPP
#define KCD2S_SDK_IDL_HPP

#include <optional>
#include <string>
#include <vector>

namespace kcd2s::sdk::idl {

enum class Side { Shared, Client, Server };
enum class Stability { Stable, Beta, Experimental };

struct Param {
  std::string name;
  std::string type;  // IDL type name (void/bool/int/float/string/vec3/quat/entity/any or a type)
};

// A copy-paste snippet from an "@example [lang] [side]" doc block.
struct Example {
  std::string lang = "ts";
  std::string side;  // empty: the member's own side
  std::string code;
};

// "///" doc comment lines in front of a type or member. Untagged lines are the summary; tags:
//   @param <name> <text>   @throws <text>   @raises <event>[, <event>]   @group <name> (types)
//   @kcds (a KCDS addition, not a base-game API)   @example [lang] [side] (the code follows below)
struct Doc {
  std::string summary;
  std::vector<std::pair<std::string, std::string>> params;  // name, description
  std::vector<std::string> throws;
  std::vector<std::string> raises;
  std::vector<Example> examples;
  std::string group;
  bool kcds = false;
};

struct Prop {
  std::string name;
  std::string type;
  Side side = Side::Shared;
  Stability stability = Stability::Stable;
  std::string since;
  bool readonly = false;
  Doc doc;
};

struct Method {
  std::string name;
  std::string ret = "void";
  std::vector<Param> params;
  Side side = Side::Shared;
  Stability stability = Stability::Stable;
  std::string since;
  Doc doc;
};

struct Event {
  std::string name;
  std::vector<Param> params;
  Side side = Side::Shared;
  Doc doc;
};

struct TypeDef {
  std::string name;
  std::string base;  // empty if none
  std::vector<Prop> props;
  std::vector<Method> methods;
  std::vector<Event> events;
  Doc doc;
};

struct ApiModel {
  std::string version;  // e.g. "1.0"
  std::vector<TypeDef> types;
};

// Parse the textual IDL. On failure returns nullopt and (if err) a human-readable message.
std::optional<ApiModel> parse_idl(const std::string& text, std::string* err = nullptr);

// Parse the lines of one "///" doc comment (without the "///" and its first space).
Doc parse_doc(const std::vector<std::string>& lines);

const char* side_name(Side s);
const char* stability_name(Stability s);

}  // namespace kcd2s::sdk::idl

#endif  // KCD2S_SDK_IDL_HPP
