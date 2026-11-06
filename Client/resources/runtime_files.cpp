#include "runtime_files.hpp"

#include <algorithm>
#include <system_error>

namespace kcds::resources::files {

namespace {

namespace fs = std::filesystem;

bool allowed_character(wchar_t character) {
  const bool letter =
      (character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z');
  const bool digit = character >= L'0' && character <= L'9';
  return letter || digit || character == L'.' || character == L'-' || character == L'_' ||
         character == L'/';
}

// The relative path a request names: its text up to a query or fragment, when every segment is
// a plain name.
std::optional<std::wstring> relative_path(const std::wstring& rest) {
  const std::wstring path = rest.substr(0, rest.find_first_of(L"?#"));
  if (path.empty()) {
    return std::nullopt;
  }
  for (const wchar_t character : path) {
    if (!allowed_character(character)) {
      return std::nullopt;
    }
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = std::min(path.find(L'/', start), path.size());
    const std::wstring segment = path.substr(start, end - start);
    if (segment.empty() || segment == L"." || segment == L"..") {
      return std::nullopt;
    }
    start = end + 1;
  }
  return path;
}

}  // namespace

std::optional<RuntimeFile> find(const std::wstring& url, const std::wstring& origin,
                                const fs::path& folder) {
  const std::wstring prefix = origin + kDotnetPath;
  if (url.size() <= prefix.size() || url.compare(0, prefix.size(), prefix) != 0) {
    return std::nullopt;
  }
  const auto relative = relative_path(url.substr(prefix.size()));
  if (!relative) {
    return std::nullopt;
  }
  std::error_code error;
  const fs::path root = fs::weakly_canonical(folder, error);
  if (error) {
    return std::nullopt;
  }
  const fs::path file = fs::weakly_canonical(root / fs::path(*relative), error);
  if (error || !fs::is_regular_file(file, error)) {
    return std::nullopt;
  }
  // The file must stay inside the folder (a link cannot lead out of it).
  const fs::path inside = file.lexically_relative(root);
  if (inside.empty() || *inside.begin() == L"..") {
    return std::nullopt;
  }
  return RuntimeFile{file, content_type_of(file)};
}

std::wstring content_type_of(const fs::path& path) {
  const std::wstring extension = path.extension().wstring();
  if (extension == L".js" || extension == L".mjs") {
    return L"text/javascript";
  }
  if (extension == L".wasm") {
    return L"application/wasm";
  }
  if (extension == L".json") {
    return L"application/json";
  }
  return L"application/octet-stream";
}

}  // namespace kcds::resources::files
