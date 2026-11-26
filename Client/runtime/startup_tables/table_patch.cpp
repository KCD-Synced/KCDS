#include "table_patch.hpp"

#include <algorithm>
#include <cctype>

namespace kcds::runtime::startup_tables {
namespace {

constexpr const char* kTables = "libs/tables/";
// Tables a running game loads patches of (crafting/recipe_tables), lower case.
constexpr const char* kLiveTables[] = {"blacksmithrecipes", "alchemyrecipe",
                                       "blacksmithworkpieces"};

}  // namespace

bool is_table_patch(const std::string& path) {
  std::string game_path = path;
  std::transform(game_path.begin(), game_path.end(), game_path.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  std::replace(game_path.begin(), game_path.end(), '\\', '/');
  const std::string root = kTables;
  if (game_path.rfind(root, 0) != 0) {
    return false;
  }
  const std::string rest = game_path.substr(root.size());
  const std::size_t slash = rest.rfind('/');
  // At least one folder under Libs/Tables, and no empty or parent folder in the path.
  if (slash == std::string::npos || slash == 0 || rest.find("//") != std::string::npos ||
      rest.find("..") != std::string::npos) {
    return false;
  }
  const std::string file = rest.substr(slash + 1);
  const bool xml = file.size() > 4 && file.compare(file.size() - 4, 4, ".xml") == 0;
  const std::size_t patch = file.find("__");
  if (!xml || patch == std::string::npos || patch == 0 || patch + 2 >= file.size() - 4) {
    return false;
  }
  const std::string table = file.substr(0, patch);
  for (const char* live : kLiveTables) {
    if (table == live) {
      return false;
    }
  }
  return true;
}

}  // namespace kcds::runtime::startup_tables
