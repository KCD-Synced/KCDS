#include "runtime.hpp"
#include "test_passes.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
  try {
    if (argc != 3 && argc != 4)
      throw std::runtime_error("Usage: client-resource-probe <port> <runtime-exe>");
    kcd2s::local::Winsock winsock;
    const auto port = std::stoul(argv[1]);
    if (port == 0 || port > 65535)
      throw std::runtime_error("Invalid test port");
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[2], -1, nullptr, 0);
    if (!size)
      throw std::runtime_error("Invalid runtime executable path");
    std::wstring executable(std::size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[2], -1, executable.data(), size);
    executable.pop_back();
    kcd2s::local::Client a("127.0.0.1", static_cast<unsigned short>(port));
    kcd2s::local::Client b("127.0.0.1", static_cast<unsigned short>(port));
    if (auto passes = kcd2s::local::test_passes(port)) {
      a.set_licence_provider(passes);
      b.set_licence_provider(passes);
    }
    a.start_resource_runtime(executable);
    b.start_resource_runtime(executable);
    bool seen_a = false, seen_b = false;
    unsigned counts_a[2]{}, counts_b[2]{};
    double completed_at = 0;
    bool reload_a = argc == 3, reload_b = argc == 3;
    const auto start = kcd2s::local::clock_seconds();
    while (kcd2s::local::clock_seconds() - start < 30) {
      const auto now = kcd2s::local::clock_seconds();
      a.tick(now);
      b.tick(now);
      for (auto* client : {&a, &b}) {
        if (client->peer().world_config() && !client->peer().world_ready())
          client->peer().mark_world_ready(client->peer().world_config()->revision);
      }
      for (const auto& message : a.peer().take_chat()) {
        std::cout << "A " << message.text << std::endl;
        if (message.text == "resource-roundtrip-ok" || message.text == "resource-roundtrip-ok-1")
          seen_a = true;
        if (message.text == "resource-roundtrip-ok-2")
          reload_a = true;
        if (message.text == "resource-roundtrip-ok-1" && ++counts_a[0] > 1)
          throw std::runtime_error("Duplicate initial resource callback");
        if (message.text == "resource-roundtrip-ok-2" && ++counts_a[1] > 1)
          throw std::runtime_error("Duplicate reloaded resource callback");
      }
      for (const auto& message : b.peer().take_chat()) {
        std::cout << "B " << message.text << std::endl;
        if (message.text == "resource-roundtrip-ok" || message.text == "resource-roundtrip-ok-1")
          seen_b = true;
        if (message.text == "resource-roundtrip-ok-2")
          reload_b = true;
        if (message.text == "resource-roundtrip-ok-1" && ++counts_b[0] > 1)
          throw std::runtime_error("Duplicate initial resource callback");
        if (message.text == "resource-roundtrip-ok-2" && ++counts_b[1] > 1)
          throw std::runtime_error("Duplicate reloaded resource callback");
      }
      if (seen_a && seen_b && reload_a && reload_b && a.resources_ready() && b.resources_ready()) {
        if (!completed_at)
          completed_at = now;
        if (now - completed_at < 0.5)
          continue;
        std::cout
            << "PASS two real client runtimes: streamed code, verified files, Events roundtrip"
            << std::endl;
        a.disconnect();
        b.disconnect();
        return 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cerr << "FAIL client resource roundtrip A=" << seen_a << " B=" << seen_b << std::endl;
    a.disconnect();
    b.disconnect();
    return 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
