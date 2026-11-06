#pragma once

#include <cstdint>

namespace kcds::runtime::characters::pipeline_metrics {

// Optional startup diagnostics. Both test flags must equal 1. A failed resolution
// installs no native hook; it does not make gameplay dependent on diagnostics.
bool install();

// Trivial locals allow the existing Present detour to restore diagnostic TLS in
// __finally under /EHsc, without catching native exceptions or changing HRESULT.
struct PresentSpan {
  std::int64_t begin = 0;
  std::int64_t overlay_end = 0;
  std::int64_t end = 0;
  bool tracked = false;
  bool previous = false;
  bool outer = false;
  bool overlay_attempted = false;
};

PresentSpan begin_present(bool test_call);
void overlay_finished(PresentSpan& span, bool attempted);
void native_present_finished(PresentSpan& span);
void leave_present(const PresentSpan& span, bool completed);
void publish_present(const PresentSpan& span);

}  // namespace kcds::runtime::characters::pipeline_metrics
