#include "store_copy.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace kcds::runtime::launch {
namespace {

namespace fs = std::filesystem;

// Epic manifests are a few KiB; anything far larger is not one.
constexpr std::uintmax_t kMaximumManifestBytes = 1024 * 1024;

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The string value of a top-level-looking "key": "value" pair (JSON escapes undone), or nullopt.
// The Epic files are flat objects of strings, numbers and booleans; this reads only strings.
std::optional<std::string> string_field(std::string_view json, std::string_view key) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  for (std::size_t at = json.find(quoted); at != std::string_view::npos;
       at = json.find(quoted, at + 1)) {
    std::size_t i = at + quoted.size();
    while (i < json.size() && is_space(json[i])) {
      ++i;
    }
    if (i >= json.size() || json[i] != ':') {
      continue;
    }
    ++i;
    while (i < json.size() && is_space(json[i])) {
      ++i;
    }
    if (i >= json.size() || json[i] != '"') {
      return std::nullopt;
    }
    std::string value;
    for (++i; i < json.size(); ++i) {
      const char c = json[i];
      if (c == '"') {
        return value;
      }
      if (c != '\\') {
        value.push_back(c);
        continue;
      }
      if (++i >= json.size()) {
        return std::nullopt;
      }
      const char escaped = json[i];
      if (escaped == '"' || escaped == '\\' || escaped == '/') {
        value.push_back(escaped);
      } else {
        // \n, \t, \uXXXX and the rest never appear in the ids and paths read here.
        return std::nullopt;
      }
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::string read_small_file(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size == 0 || size > kMaximumManifestBytes) {
    return {};
  }
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool has_file_with_extension(const fs::path& folder, const std::string& prefix,
                             const fs::path& extension) {
  std::error_code ec;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path name = it->path().filename();
    if (name.extension() == extension && name.string().rfind(prefix, 0) == 0 &&
        it->is_regular_file(ec)) {
      return true;
    }
  }
  return false;
}

bool path_exists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

void append_uri_component(std::string& out, const std::string& text) {
  constexpr char kHex[] = "0123456789ABCDEF";
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0F]);
    }
  }
}

}  // namespace

const char* store_name(Store store) {
  switch (store) {
    case Store::Steam:
      return "Steam";
    case Store::Epic:
      return "Epic Games Store";
    case Store::Gog:
      return "GOG";
    case Store::MsStore:
      return "Microsoft Store";
  }
  return "Steam";
}

Store store_of_install(const fs::path& root) {
  if (path_exists(root / "appxmanifest.xml") || path_exists(root / "MicrosoftGame.config")) {
    return Store::MsStore;
  }
  if (path_exists(root / ".egstore")) {
    return Store::Epic;
  }
  if (has_file_with_extension(root, "goggame-", ".info")) {
    return Store::Gog;
  }
  return Store::Steam;
}

std::optional<EpicIds> parse_epic_ids(std::string_view json) {
  auto app = string_field(json, "AppName");
  auto catalog_namespace = string_field(json, "CatalogNamespace");
  auto item = string_field(json, "CatalogItemId");
  if (!app || !catalog_namespace || !item || app->empty() || catalog_namespace->empty() ||
      item->empty()) {
    return std::nullopt;
  }
  return EpicIds{*app, *catalog_namespace, *item};
}

std::string epic_install_location(std::string_view json) {
  return string_field(json, "InstallLocation").value_or(std::string());
}

std::string epic_launch_uri(const EpicIds& ids) {
  std::string uri = "com.epicgames.launcher://apps/";
  append_uri_component(uri, ids.catalog_namespace);
  uri += "%3A";
  append_uri_component(uri, ids.catalog_item);
  uri += "%3A";
  append_uri_component(uri, ids.app_name);
  uri += "?action=launch&silent=true";
  return uri;
}

std::optional<EpicIds> find_epic_ids(const fs::path& root, const fs::path& program_data) {
  std::error_code ec;
  for (fs::directory_iterator it(root / ".egstore", ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() == ".mancpn") {
      if (auto ids = parse_epic_ids(read_small_file(it->path()))) {
        return ids;
      }
    }
  }
  // The Epic launcher's own record of the install.
  const fs::path manifests = program_data / "Epic" / "EpicGamesLauncher" / "Data" / "Manifests";
  ec.clear();
  for (fs::directory_iterator it(manifests, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".item") {
      continue;
    }
    const std::string text = read_small_file(it->path());
    const std::string location = epic_install_location(text);
    std::error_code same_ec;
    const fs::path location_path(std::u8string(location.begin(), location.end()));
    if (!location.empty() && fs::equivalent(location_path, root, same_ec)) {
      if (auto ids = parse_epic_ids(text)) {
        return ids;
      }
    }
  }
  return std::nullopt;
}

}  // namespace kcds::runtime::launch
