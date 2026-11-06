#pragma once
// The server's client resource files over HTTP. A server whose
// catalogInfo says httpFiles serves each file of its catalog by its SHA-256 at
// http://<server>:<game port>/kcds/files/<hash> (TCP, the game's own port number). The client
// downloads a file it lacks whole there, keeps it in the download cache once its hash is right,
// and answers the resource runtime's chunk requests from the cache; a file the HTTP way cannot
// bring is asked from the server in chunks over the game connection, as before.
//
// The downloader is the platform's (the game client's: WinHTTP); this interface keeps the runtime
// portable. Started on the network thread, it works on its own threads and hands back what ended.
#include <filesystem>
#include <string>
#include <vector>

namespace kcd2s::local::resources {

struct HttpFile {
  std::string hash;
  bool ok = false;
  // The whole file, downloaded into the cache's folder under a temporary name, when ok.
  std::filesystem::path path;
};

class HttpFiles {
 public:
  virtual ~HttpFiles() = default;
  // The token the server gave this game for its HTTP files (catalogInfo httpToken), sent with
  // every request; the server answers no game that has none.
  virtual void set_token(const std::string& token) = 0;
  // Starts the download of the file with this hash (once per hash at a time).
  virtual void start(const std::string& hash) = 0;
  // The downloads that ended since the last call.
  virtual std::vector<HttpFile> take_done() = 0;
};

}  // namespace kcd2s::local::resources
