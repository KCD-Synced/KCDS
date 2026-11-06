// Shared entry point for every KCD2-Synced test executable.
// Runs all registered cases, prints a summary, exits non-zero on any failure.
#include <cstdio>
#include <cstring>

#include "kcd2s/testkit.hpp"

int main(int argc, char** argv) {
  const char* filter = (argc > 1) ? argv[1] : nullptr;  // optional substring filter on "suite.name"

  int ran = 0, passed = 0, failed = 0;
  for (auto& c : kcd2s::testkit::registry()) {
    std::string full = c.suite + "." + c.name;
    if (filter && full.find(filter) == std::string::npos)
      continue;

    kcd2s::testkit::Context ctx;
    ctx.suite = c.suite.c_str();
    ctx.name = c.name.c_str();
    std::printf("[ RUN  ] %s\n", full.c_str());
    try {
      c.fn(ctx);
    } catch (const std::exception& e) {
      ctx.fail(__FILE__, __LINE__, std::string("threw std::exception: ") + e.what());
    } catch (...) {
      ctx.fail(__FILE__, __LINE__, "threw unknown exception");
    }
    ++ran;
    if (ctx.failures == 0) {
      ++passed;
      std::printf("[  OK  ] %s\n", full.c_str());
    } else {
      ++failed;
      std::printf("[ FAIL ] %s (%d assertion(s))\n", full.c_str(), ctx.failures);
    }
  }

  std::printf("\n%d run, %d passed, %d failed\n", ran, passed, failed);
  if (ran == 0) {
    std::fprintf(stderr, "no tests matched\n");
    return 2;
  }
  return failed == 0 ? 0 : 1;
}
