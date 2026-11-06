// The game client's downloader of a server's resource files over HTTP (the
// runtime's side: kcd2s/local/resources/http_files.hpp). Each file is asked at
// http://<server ip>:<game port>/kcds/files/<sha256> with WinHTTP on a worker thread, a few at
// once, and written to a file as it comes (a file may be hundreds of megabytes); the cache keeps
// it only when its SHA-256 is the file's (so plain HTTP is fine: no other bytes can get in).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "../../../Backend/integration/local/resources/http_files.hpp"

namespace kcds::runtime::session {

// A downloader of this server's files into `folder` (the download cache's, so a finished file is
// moved into place there).
std::unique_ptr<kcd2s::local::resources::HttpFiles> make_resource_files(
    const std::string& ip, std::uint16_t port, const std::filesystem::path& folder);

}  // namespace kcds::runtime::session
