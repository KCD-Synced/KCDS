#include <fstream>
#include <sstream>
#include <string>

#include "kcd2s/sdk/gen.hpp"
#include "kcd2s/sdk/idl.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s::sdk;

static bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

KCD2S_TEST(idl, parses_model) {
  const std::string src = R"(
    api 1.0
    type Entity {
      prop id: entity @readonly
      prop health: float @server @beta @since(1.2)
    }
    type Player : Entity {
      method kick(reason: string): void @server
      event onChat(message: string)
    }
  )";
  std::string err;
  auto m = idl::parse_idl(src, &err);
  KCD2S_CHECK(m.has_value());
  if (!m)
    return;
  KCD2S_CHECK_EQ(m->version, std::string("1.0"));
  KCD2S_CHECK_EQ(m->types.size(), std::size_t{2});

  const auto& ent = m->types[0];
  KCD2S_CHECK_EQ(ent.name, std::string("Entity"));
  KCD2S_CHECK_EQ(ent.props.size(), std::size_t{2});
  KCD2S_CHECK(ent.props[0].readonly);
  KCD2S_CHECK(ent.props[1].side == idl::Side::Server);
  KCD2S_CHECK(ent.props[1].stability == idl::Stability::Beta);
  KCD2S_CHECK_EQ(ent.props[1].since, std::string("1.2"));

  const auto& pl = m->types[1];
  KCD2S_CHECK_EQ(pl.base, std::string("Entity"));
  KCD2S_CHECK_EQ(pl.methods.size(), std::size_t{1});
  KCD2S_CHECK_EQ(pl.methods[0].params.size(), std::size_t{1});
  KCD2S_CHECK_EQ(pl.events.size(), std::size_t{1});
}

KCD2S_TEST(idl, rejects_malformed) {
  std::string err;
  KCD2S_CHECK(!idl::parse_idl("type Player {", &err).has_value());  // unclosed
  KCD2S_CHECK(!err.empty());
  KCD2S_CHECK(!idl::parse_idl("type { }", nullptr).has_value());  // missing name
  KCD2S_CHECK(!idl::parse_idl("wat Player {}", nullptr).has_value());
}

KCD2S_TEST(gen, typescript_output) {
  const std::string src = R"(
    api 1.0
    type Entity { prop position: vec3 @stable }
    type Player : Entity {
      prop name: string @server @readonly
      method kick(reason: string): void @server
      event onChat(message: string)
    }
  )";
  auto m = idl::parse_idl(src);
  KCD2S_CHECK(m.has_value());
  const std::string ts = gen::emit_typescript(*m);

  KCD2S_CHECK(contains(ts, "export type EntityId = number;"));
  KCD2S_CHECK(contains(ts, "export interface Vec3"));
  KCD2S_CHECK(contains(ts, "export interface Entity {"));
  KCD2S_CHECK(contains(ts, "position: Vec3;"));
  KCD2S_CHECK(contains(ts, "export interface Player extends Entity {"));
  KCD2S_CHECK(contains(ts, "readonly name: string;"));
  KCD2S_CHECK(contains(ts, "kick(reason: string): void;"));
  KCD2S_CHECK(contains(ts, "on(event: \"onChat\", cb: (message: string) => void): void;"));
  KCD2S_CHECK(contains(ts, "@server"));
}

KCD2S_TEST(gen, lua_output) {
  const std::string src = R"(
    api 1.0
    type Entity { prop position: vec3 }
    type Player : Entity {
      prop name: string @readonly
      method kick(reason: string): void
      method getPing(): int
      event onChat(message: string)
    }
  )";
  auto m = idl::parse_idl(src);
  KCD2S_CHECK(m.has_value());
  const std::string lua = gen::emit_lua(*m);
  KCD2S_CHECK(contains(lua, "---@meta"));
  KCD2S_CHECK(contains(lua, "---@class Player : Entity"));
  KCD2S_CHECK(contains(lua, "---@field name string"));
  KCD2S_CHECK(contains(lua, "---@param reason string"));
  KCD2S_CHECK(contains(lua, "function Player:kick(reason) end"));
  KCD2S_CHECK(contains(lua, "---@return integer"));  // getPing
  KCD2S_CHECK(contains(lua, "event onChat"));
}

KCD2S_TEST(gen, csharp_output) {
  const std::string src = R"(
    api 1.0
    type Entity { prop health: float @server }
    type Player : Entity {
      prop name: string @readonly
      method kick(reason: string): void
      event onChat(message: string)
    }
  )";
  auto m = idl::parse_idl(src);
  KCD2S_CHECK(m.has_value());
  const std::string cs = gen::emit_csharp(*m);
  KCD2S_CHECK(contains(cs, "public partial class Player : Entity {"));
  KCD2S_CHECK(contains(cs, "public float health { get; set; }"));
  KCD2S_CHECK(contains(cs, "public string name { get; }"));  // readonly -> no setter
  KCD2S_CHECK(contains(cs, "public void kick(string reason);"));
  KCD2S_CHECK(contains(cs, "public event Action<string> onChat;"));
}

KCD2S_TEST(idl, doc_comments) {
  const std::string src = R"(
    api 1.0
    /// A connected player.
    /// @group Entities
    type Player {
      /// Removes the player from the server.
      /// @param reason Shown to the player,
      ///   in their own language.
      /// @throws When the player has already left.
      /// @raises playerDisconnect
      /// @kcds
      /// @example ts server
      /// if (player.name === "griefer") {
      ///   player.kick("Bye");
      /// }
      method kick(reason: string): void @server
      # an ordinary comment is not a doc comment
      prop name: string @readonly
    }
  )";
  std::string err;
  auto m = idl::parse_idl(src, &err);
  KCD2S_CHECK(m.has_value());
  if (!m)
    return;
  const auto& player = m->types[0];
  KCD2S_CHECK_EQ(player.doc.summary, std::string("A connected player."));
  KCD2S_CHECK_EQ(player.doc.group, std::string("Entities"));
  const auto& kick = player.methods[0].doc;
  KCD2S_CHECK_EQ(kick.summary, std::string("Removes the player from the server."));
  KCD2S_CHECK_EQ(kick.params.size(), std::size_t{1});
  KCD2S_CHECK_EQ(kick.params[0].second, std::string("Shown to the player, in their own language."));
  KCD2S_CHECK_EQ(kick.throws.size(), std::size_t{1});
  KCD2S_CHECK_EQ(kick.raises.size(), std::size_t{1});
  KCD2S_CHECK(kick.kcds);
  KCD2S_CHECK_EQ(kick.examples.size(), std::size_t{1});
  KCD2S_CHECK_EQ(kick.examples[0].side, std::string("server"));
  KCD2S_CHECK_EQ(kick.examples[0].code,
                 std::string("if (player.name === \"griefer\") {\n  player.kick(\"Bye\");\n}"));
  KCD2S_CHECK(player.props[0].doc.summary.empty());
}

KCD2S_TEST(gen, docs_json_output) {
  const std::string src = R"(
    api 1.0
    /// A connected player.
    /// @group Entities
    type Player {
      /// Removes the "player".
      /// @param reason Shown to the player.
      method kick(reason: string): void @server @since(1.0)
      prop name: string @readonly
      event onChat(message: string)
    }
    type Empty { }
  )";
  auto m = idl::parse_idl(src);
  KCD2S_CHECK(m.has_value());
  const std::string json = gen::emit_docs_json(*m);
  KCD2S_CHECK(contains(json, "\"schema\": 1"));
  KCD2S_CHECK(contains(json, "\"apiVersion\": \"1.0\""));
  KCD2S_CHECK(contains(json, "\"group\": \"Entities\""));
  KCD2S_CHECK(contains(json, "\"signature\": \"kick(reason: string): void\""));
  KCD2S_CHECK(contains(json, "\"summary\": \"Removes the \\\"player\\\".\""));
  KCD2S_CHECK(contains(json, "\"description\": \"Shown to the player.\""));
  KCD2S_CHECK(contains(json, "\"signature\": \"readonly name: string\""));
  KCD2S_CHECK(contains(json, "on(event: \\\"onChat\\\", cb: (message: string) => void): void"));
  KCD2S_CHECK(contains(json, "\"members\": []}"));
  // the summary reaches the .d.ts too
  KCD2S_CHECK(contains(gen::emit_typescript(*m),
                       "/** Removes the \"player\". @server @stable @since 1.0 */"));
}

#ifdef KCD2S_API_IDL
KCD2S_TEST(gen, real_api_idl_parses_and_emits) {
  std::ifstream f(KCD2S_API_IDL);
  KCD2S_CHECK(f.good());
  if (!f.good())
    return;
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string err;
  auto m = idl::parse_idl(ss.str(), &err);
  KCD2S_CHECK(m.has_value());
  if (!m)
    return;
  KCD2S_CHECK(m->types.size() >= 3);  // Entity, Player, World
  const std::string ts = gen::emit_typescript(*m);
  KCD2S_CHECK(contains(ts, "export interface Player extends Entity {"));
  KCD2S_CHECK(contains(ts, "export interface World {"));
  // All three emitters consume the same model without error.
  KCD2S_CHECK(contains(gen::emit_lua(*m), "---@class Player : Entity"));
  KCD2S_CHECK(contains(gen::emit_csharp(*m), "public partial class World"));
  KCD2S_CHECK(contains(gen::emit_docs_json(*m), "\"name\": \"World\""));
}
#endif
