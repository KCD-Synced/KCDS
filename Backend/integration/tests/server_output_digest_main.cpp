// tool_integration_server_digest: prints the digest of what the server sends in a fixed session
// (server_output_digest.hpp). Run it on two builds with the same options; the digests must match
// when a change must not change what the game receives.
//
//   tool_integration_server_digest [--clients N] [--seconds S] [--workers W] [--loss L]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "server_output_digest.hpp"

int main(int argc, char** argv) {
  kcds::digest::DigestOptions options;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const char* value = argv[i + 1];
    if (key == "--clients") {
      options.clients = unsigned(std::strtoul(value, nullptr, 10));
    } else if (key == "--seconds") {
      options.seconds = unsigned(std::strtoul(value, nullptr, 10));
    } else if (key == "--workers") {
      options.workers = unsigned(std::strtoul(value, nullptr, 10));
    } else if (key == "--loss") {
      options.loss = std::strtod(value, nullptr);
    } else if (key == "--send-batching") {
      options.send_batching = std::strtoul(value, nullptr, 10) != 0;
    } else {
      std::fprintf(stderr,
                   "usage: tool_integration_server_digest [--clients N] [--seconds S] "
                   "[--workers W] [--loss L] [--send-batching 0|1]\n");
      return 2;
    }
  }
  const auto started = std::chrono::steady_clock::now();
  const auto digest = kcds::digest::run_digest(options);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf(
      "SERVER_OUTPUT_DIGEST {\"clients\":%u,\"seconds\":%u,\"workers\":%u,\"loss\":%.3f,"
      "\"digest\":\"%016llx\",\"datagrams\":%llu,\"bytes\":%llu,\"mirrored_entities\":%llu,"
      "\"server_seconds\":%.2f,\"run_seconds\":%.2f}\n",
      options.clients, options.seconds, options.workers, options.loss,
      static_cast<unsigned long long>(digest.value),
      static_cast<unsigned long long>(digest.datagrams),
      static_cast<unsigned long long>(digest.bytes),
      static_cast<unsigned long long>(digest.mirrored_entities), digest.server_seconds, seconds);
  return 0;
}
