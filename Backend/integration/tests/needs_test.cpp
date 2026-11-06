#include "kcd2s/integration/needs.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

KCD2S_TEST(needs, round_trip) {
  for (const bool enabled : {true, false}) {
    bool decoded = !enabled;
    KCD2S_CHECK(decode_needs(encode_needs(enabled), decoded));
    KCD2S_CHECK(decoded == enabled);
  }
  bool decoded = true;
  KCD2S_CHECK(!decode_needs({kNeedsTag, 2}, decoded));
  KCD2S_CHECK(!decode_needs({kNeedsTag}, decoded));
  KCD2S_CHECK(!decode_needs({kNeedsTag, 0, 0}, decoded));
  KCD2S_CHECK(!decode_needs({kNeedsTag + 1, 0}, decoded));
  KCD2S_CHECK(decoded);
}
