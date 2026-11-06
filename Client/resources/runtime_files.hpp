#pragma once

#include <filesystem>
#include <optional>
#include <string>

// The resource runtime's own files: the .NET WebAssembly runtime C# client
// resources run on ships next to kcd2s-resource-runtime.exe in its dotnet folder, and the
// runtime page reads it from its own origin, under https://runtime.kcds.invalid/dotnet/. Only
// files inside that folder are served; every other request stays blocked.
namespace kcds::resources::files {

// The path under the runtime's origin the .NET files are served from.
inline constexpr const wchar_t* kDotnetPath = L"dotnet/";

// A file the runtime page may read: where it is and its media type.
struct RuntimeFile {
  std::filesystem::path path;
  std::wstring content_type;
};

// The file `url` names, when it is `origin` + dotnet/ + a path inside `folder` that exists:
// letters, digits, '.', '-', '_' and '/' only, no empty or dot segments, query and fragment
// ignored. Nothing for any other URL.
std::optional<RuntimeFile> find(const std::wstring& url, const std::wstring& origin,
                                const std::filesystem::path& folder);

// The media type the page needs for a file of the .NET runtime (WebAssembly must be
// application/wasm to compile while it downloads).
std::wstring content_type_of(const std::filesystem::path& path);

}  // namespace kcds::resources::files
