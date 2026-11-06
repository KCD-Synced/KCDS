// The origin rule of the update feed and the crash-reporting DSN, so both endpoints follow
// exactly one rule.
#pragma once

#include <string>

namespace kcds::launcher::feed {

// True when origin is a bare https origin (or a loopback http origin when allow_http_loopback is
// set), producing the normalised form.
bool normalize_origin(const std::string& origin, bool allow_http_loopback,
                      std::string& normalized_origin);

}  // namespace kcds::launcher::feed
