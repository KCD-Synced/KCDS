// kcd2s-gen — read the canonical IDL and emit per-language SDK bindings.
//   kcd2s-gen <api.idl> <out-dir>
// Writes <out-dir>/ts/kcd2s.d.ts, <out-dir>/lua/kcd2s.lua, <out-dir>/csharp/Kcd2Synced.Sdk.cs and
// <out-dir>/docs/api.json (the website's API reference).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "kcd2s/sdk/gen.hpp"
#include "kcd2s/sdk/idl.hpp"

namespace {
bool write_file(const std::filesystem::path& p, const std::string& content) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  if (!f)
    return false;
  f << content;
  return static_cast<bool>(f);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: kcd2s-gen <api.idl> <out-dir>\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  std::ostringstream ss;
  ss << in.rdbuf();

  std::string err;
  auto model = kcd2s::sdk::idl::parse_idl(ss.str(), &err);
  if (!model) {
    std::fprintf(stderr, "IDL parse error: %s\n", err.c_str());
    return 1;
  }

  const std::filesystem::path out(argv[2]);
  using namespace kcd2s::sdk::gen;
  bool ok = write_file(out / "ts" / "kcd2s.d.ts", emit_typescript(*model));
  ok = write_file(out / "lua" / "kcd2s.lua", emit_lua(*model)) && ok;
  ok = write_file(out / "csharp" / "Kcd2Synced.Sdk.cs", emit_csharp(*model)) && ok;
  ok = write_file(out / "docs" / "api.json", emit_docs_json(*model)) && ok;
  if (!ok) {
    std::fprintf(stderr, "failed to write one or more outputs\n");
    return 1;
  }
  std::printf("kcd2s-gen: emitted TS/Lua/C#/docs for %zu types (API v%s) to %s\n",
              model->types.size(), model->version.c_str(), argv[2]);
  return 0;
}
