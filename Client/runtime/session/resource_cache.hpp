// The game client's folder of a server's downloaded resource files: each file is
// named by its SHA-256 (kcd2s/local/resources/download_cache.hpp keeps it). A big file stays
// there: the resource runtime hands the game its hash ("cached": <hash>) and
// the game takes the bytes from this folder for the movie loader, a custom map or a page.
#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace kcds::runtime::session {

// %LOCALAPPDATA%\KCDS\cache\resources (the temporary folder's when that is not set).
std::filesystem::path resource_cache_folder();

// The hashes of the current server's verified catalog (runtime.hpp, none until it is verified):
// the only cached files this session hands out, so one server's scripts never reach another
// server's files on this machine. Any thread.
void set_catalog_hashes(const std::set<std::string>& hashes);

// The cached file of this SHA-256 (64 lowercase hex) when it is one of the current server's, or
// an empty path.
std::filesystem::path cached_resource_file(const std::string& hash);

}  // namespace kcds::runtime::session
