#include "kcd2s/integration/departure.hpp"

#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::integration::departure_reason_name;
using kcd2s::integration::DepartureReason;
using kcd2s::integration::DepartureSteps;
using kcd2s::integration::run_departure;

namespace {

class RecordingSteps : public DepartureSteps {
 public:
  void notify_resources() override {
    calls.push_back("notify_resources");
  }
  void despawn_avatar() override {
    calls.push_back("despawn_avatar");
  }
  void remove_client() override {
    calls.push_back("remove_client");
  }
  void forget_address() override {
    calls.push_back("forget_address");
  }
  void log_leave(DepartureReason reason) override {
    calls.push_back("log_leave");
    reasons.push_back(reason);
  }

  std::vector<std::string> calls;
  std::vector<DepartureReason> reasons;
};

}  // namespace

KCD2S_TEST(departure, timeout_runs_the_documented_order) {
  RecordingSteps steps;
  run_departure(steps, DepartureReason::Timeout);
  const std::vector<std::string> expected = {
      "notify_resources", "despawn_avatar", "remove_client", "forget_address", "log_leave",
  };
  KCD2S_CHECK(steps.calls == expected);
  KCD2S_CHECK(steps.reasons.size() == 1 && steps.reasons[0] == DepartureReason::Timeout);
}

KCD2S_TEST(departure, disconnect_runs_exactly_the_timeout_steps) {
  RecordingSteps timeout;
  RecordingSteps disconnect;
  run_departure(timeout, DepartureReason::Timeout);
  run_departure(disconnect, DepartureReason::Disconnect);
  KCD2S_CHECK(disconnect.calls == timeout.calls);
  KCD2S_CHECK(disconnect.reasons.size() == 1 &&
              disconnect.reasons[0] == DepartureReason::Disconnect);
}

KCD2S_TEST(departure, reason_names_are_stable_log_tokens) {
  const std::string timeout = departure_reason_name(DepartureReason::Timeout);
  const std::string disconnect = departure_reason_name(DepartureReason::Disconnect);
  KCD2S_CHECK(timeout == "timeout");
  KCD2S_CHECK(disconnect == "disconnect");
}
