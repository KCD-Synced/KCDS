#include "runtime_files.hpp"

#include <filesystem>
#include <fstream>

#include "kcd2s/testkit.hpp"

namespace {

namespace fs = std::filesystem;
using kcds::resources::files::content_type_of;
using kcds::resources::files::find;

constexpr const wchar_t* kOrigin = L"https://runtime.kcds.invalid/";

fs::path fresh_folder() {
  const fs::path base = fs::temp_directory_path() / "kcds-runtime-files-test";
  fs::remove_all(base);
  fs::create_directories(base / "dotnet" / "_framework");
  std::ofstream(base / "dotnet" / "_framework" / "dotnet.js") << "export {}";
  std::ofstream(base / "dotnet" / "_framework" / "dotnet.native.wasm") << "wasm";
  std::ofstream(base / "secret.txt") << "outside";
  return base;
}

}  // namespace

KCD2S_TEST(runtime_files, a_file_of_the_dotnet_folder_is_found_with_its_type) {
  const fs::path base = fresh_folder();
  const auto script =
      find(L"https://runtime.kcds.invalid/dotnet/_framework/dotnet.js", kOrigin, base / "dotnet");
  KCD2S_CHECK(script.has_value());
  KCD2S_CHECK(script->content_type == L"text/javascript");
  const auto wasm = find(L"https://runtime.kcds.invalid/dotnet/_framework/dotnet.native.wasm?x=1",
                         kOrigin, base / "dotnet");
  KCD2S_CHECK(wasm.has_value());
  KCD2S_CHECK(wasm->content_type == L"application/wasm");
  fs::remove_all(base);
}

KCD2S_TEST(runtime_files, nothing_outside_the_folder_or_another_origin_is_found) {
  const fs::path base = fresh_folder();
  const fs::path folder = base / "dotnet";
  KCD2S_CHECK(!find(L"https://runtime.kcds.invalid/dotnet/../secret.txt", kOrigin, folder));
  KCD2S_CHECK(!find(L"https://runtime.kcds.invalid/dotnet/%2e%2e/secret.txt", kOrigin, folder));
  KCD2S_CHECK(!find(L"https://runtime.kcds.invalid/dotnet/_framework//dotnet.js", kOrigin, folder));
  KCD2S_CHECK(!find(L"https://runtime.kcds.invalid/dotnet/_framework\\dotnet.js", kOrigin, folder));
  KCD2S_CHECK(!find(L"https://runtime.kcds.invalid/dotnet/", kOrigin, folder));
  KCD2S_CHECK(!find(L"https://runtime.kcds.invalid/dotnet/missing.js", kOrigin, folder));
  KCD2S_CHECK(!find(L"https://runtime.kcds.invalid/secret.txt", kOrigin, folder));
  KCD2S_CHECK(!find(L"https://other.invalid/dotnet/_framework/dotnet.js", kOrigin, folder));
  KCD2S_CHECK(!find(L"https://runtime.kcds.invalid/dotnet/_framework", kOrigin, folder));
  fs::remove_all(base);
}

KCD2S_TEST(runtime_files, the_media_types_the_page_needs) {
  KCD2S_CHECK(content_type_of(L"a.mjs") == L"text/javascript");
  KCD2S_CHECK(content_type_of(L"blazor.boot.json") == L"application/json");
  KCD2S_CHECK(content_type_of(L"Kcds.Sdk.dll") == L"application/octet-stream");
}
