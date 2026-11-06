// A real pipe peer for the cross-platform ScriptProcess burst/deadlock regressions.
#include "kcd2s/sdk/marshal.hpp"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using kcd2s::sdk::Value;

static void send(const Value& value) {
  const auto bytes = kcd2s::sdk::encode(value);
  static constexpr char hex[] = "0123456789abcdef";
  std::string line;
  line.reserve(bytes.size() * 2 + 1);
  for (auto byte : bytes) {
    line += hex[byte >> 4];
    line += hex[byte & 15];
  }
  line += '\n';
  std::cout << line;
}

int main(int argc, char** argv) {
#ifdef _WIN32
  // The pipe protocol uses LF delimiters; MSVCRT text translation would append a CR byte.
  if (_setmode(_fileno(stdout), _O_BINARY) == -1)
    return 4;
#endif
  if (argc != 2)
    return 2;
  if (std::string(argv[1]) == "slow") {
    // A busy host: reads nothing for a while, then counts the 64 lines the parent sent.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    std::string line;
    int lines = 0;
    while (lines < 64 && std::getline(std::cin, line)) {
      lines += 1;
    }
    send(Value(lines));
    std::cout.flush();
    return 0;
  }
  const bool overflow = std::string(argv[1]) == "overflow";
  if (overflow) {
    // Exceeds both the mailbox and OS pipe capacities. The parent must keep draining even
    // after failure or this child cannot exit; the test's bounded deadline detects that cycle.
    for (int i = 0; i < 20000; ++i)
      send(Value(std::string(1024, 'x')));
    std::cout.flush();
    return 0;
  }
  for (int id = 1; id <= 1100; ++id) {
    send(
        Value(Value::Map{{"op", "entityPosition"},
                         {"id", id},
                         {"position", Value::Map{{"x", double(id)}, {"y", 2108.0}, {"z", 14.0}}}}));
    send(Value(Value::Map{{"op", "log"}, {"id", id}, {"text", std::string(1024, 'l')}}));
    send(Value(
        Value::Map{{"op", "entityRotation"},
                   {"id", id},
                   {"rotation", Value::Map{{"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"w", 1.0}}}}));
  }
  std::cout.flush();
  std::string line;
  // Parent writes more than a pipe buffer while this child is still producing its burst.
  for (int i = 0; i < 32; ++i) {
    if (!std::getline(std::cin, line))
      return 3;
  }
  send(Value("done"));
  std::cout.flush();
  return 0;
}
