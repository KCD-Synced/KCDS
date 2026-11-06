// kcd2s-demo — run the headless end-to-end scenario and print the outcome.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kcd2s/integration/demo.hpp"

int main(int argc, char** argv) {
  kcd2s::integration::DemoConfig cfg;
  for (int i = 1; i < argc - 1; ++i) {
    if (std::strcmp(argv[i], "--clients") == 0)
      cfg.clients = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--npcs") == 0)
      cfg.npcs = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--ticks") == 0)
      cfg.ticks = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--loss") == 0)
      cfg.loss = std::atof(argv[++i]);
    else if (std::strcmp(argv[i], "--parallel") == 0)
      cfg.parallel = std::atoi(argv[++i]) != 0;
  }

  const auto r = kcd2s::integration::run_demo(cfg);
  std::printf("KCD2-Synced headless demo\n");
  std::printf("  clients=%d npcs=%d ticks=%d loss=%.2f parallel=%d\n", cfg.clients, cfg.npcs,
              cfg.ticks, cfg.loss, cfg.parallel ? 1 : 0);
  std::printf("  world live entities: %zu\n", r.world_live);
  std::printf("  converged: %s\n", r.converged ? "YES" : "NO");
  std::printf("  snapshot bytes sent: %llu (~%llu/tick)\n",
              static_cast<unsigned long long>(r.snapshot_bytes),
              static_cast<unsigned long long>(r.snapshot_bytes / (cfg.ticks ? cfg.ticks : 1)));
  std::printf("  NetBus events delivered: %d, rpc responses: %d\n", r.events_delivered, r.rpc_ok);
  std::printf("  mirror sizes:");
  for (std::size_t s : r.mirror_sizes)
    std::printf(" %zu", s);
  std::printf("\n");
  return r.converged ? 0 : 1;
}
