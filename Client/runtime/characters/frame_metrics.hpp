#pragma once

#include <cstdint>
#include <cstddef>

namespace kcds::runtime::characters::frame_metrics {

// Test-only wall-clock breakdown of the original engine update and the subsequent adapter.
// Zero when disabled; no timer queries unless both character diagnostics flags equal 1.
std::int64_t stamp();
void finish(std::int64_t begin, std::int64_t native_end, bool adapter_failed);

enum class Phase : std::size_t {
  Setup,
  Mirror,
  Local,
  LocalState,
  Retirement,
  Horses,
  Npcs,
  Features,
  Players,
  Combat,
  Other,
  Count
};

// Sequential adapter regions. Early returns are accounted for in Other; this never alters
// feature order or holds a shared lock. Timers are disabled with the same strict test flags.
class Phases {
 public:
  Phases();
  ~Phases() {
    mark(Phase::Other);
  }
  Phases(const Phases&) = delete;
  Phases& operator=(const Phases&) = delete;
  void mark(Phase phase);

 private:
  std::int64_t previous_;
};

}  // namespace kcds::runtime::characters::frame_metrics
