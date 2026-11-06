// Member departure: the single, ordered cleanup for a player leaving the server..
//
// A member leaves because the inactivity timeout expired, because a valid KSTP disconnect notice
// arrived (transport/disconnect.hpp), or because a resource kicked it. All reasons run exactly the
// same steps
// in exactly the same order through run_departure(), so a graceful disconnect is observably the
// timeout path, only sooner:
//
//   1. notify_resources  resource host gets the `disconnect` op while the avatar still exists
//   2. despawn_avatar    the avatar entity leaves the world; viewers get DESTRUCT next snapshot
//   3. remove_client     the peer's connection and ownership are released (owned entities are
//                        re-elected)
//   4. forget_address    the admission gate forgets the transport address, so a later session
//                        has to complete the cookie handshake again
//   5. log_leave         the LEAVE line, tagged with the reason
#ifndef KCD2S_INTEGRATION_DEPARTURE_HPP
#define KCD2S_INTEGRATION_DEPARTURE_HPP

#include <cstdint>

namespace kcd2s::integration {

enum class DepartureReason : std::uint8_t {
  Timeout,     // no valid packet within the inactivity timeout
  Disconnect,  // a valid disconnect notice from the member's own session
  Kicked,      // a resource kicked the player; its client was told why first
};

inline const char* departure_reason_name(DepartureReason reason) {
  switch (reason) {
    case DepartureReason::Timeout:
      return "timeout";
    case DepartureReason::Disconnect:
      return "disconnect";
    case DepartureReason::Kicked:
      return "kicked";
  }
  return "unknown";
}

// The host-specific actions behind each cleanup step, for one departing member.
class DepartureSteps {
 public:
  virtual ~DepartureSteps() = default;

  virtual void notify_resources() = 0;
  virtual void despawn_avatar() = 0;
  virtual void remove_client() = 0;
  virtual void forget_address() = 0;
  virtual void log_leave(DepartureReason reason) = 0;
};

inline void run_departure(DepartureSteps& steps, DepartureReason reason) {
  steps.notify_resources();
  steps.despawn_avatar();
  steps.remove_client();
  steps.forget_address();
  steps.log_leave(reason);
}

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_DEPARTURE_HPP
