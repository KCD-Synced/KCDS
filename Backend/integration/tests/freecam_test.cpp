#include "kcd2s/integration/freecam.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

KCD2S_TEST(freecam, round_trip) {
  for (const bool allowed : {true, false}) {
    bool decoded = !allowed;
    KCD2S_CHECK(decode_freecam(encode_freecam(allowed), decoded));
    KCD2S_CHECK(decoded == allowed);
  }
  bool decoded = true;
  KCD2S_CHECK(!decode_freecam({kFreecamTag, 2}, decoded));
  KCD2S_CHECK(!decode_freecam({kFreecamTag}, decoded));
}
