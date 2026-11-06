#pragma once

#include "licence_pass.hpp"

namespace kcds::runtime::identity {

// Obtain a Microsoft-signed licence token for the Master's challenge using the running GDK
// game's Store session. Call on a worker thread. No sign-in UI is opened. Returns "" when GDK
// is unavailable, the Store rejects the request, or the bounded async wait expires.
std::string xbox_licence_token(const std::string& challenge, Logger log);

}  // namespace kcds::runtime::identity
