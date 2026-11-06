#include "kcd2s/integration/world_time.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

KCD2S_TEST(world_time, round_trips) {
  WorldTime time;
  time.day = 412;
  time.hour = 17.25f;
  time.timeScale = 20.0f;
  WorldTime decoded;
  KCD2S_CHECK(decode_world_time(encode_world_time(time), decoded));
  KCD2S_CHECK(decoded == time);
  time.timeScale = 0.0f;
  KCD2S_CHECK(decode_world_time(encode_world_time(time), decoded));
  KCD2S_CHECK(decoded == time);
}

KCD2S_TEST(world_time, refuses_out_of_range) {
  WorldTime time;
  time.hour = 24.0f;
  KCD2S_CHECK(encode_world_time(time).empty());
  time.hour = 12.0f;
  time.timeScale = -1.0f;
  KCD2S_CHECK(encode_world_time(time).empty());
  time.timeScale = 1.0f;
  time.day = kMaxWorldDay + 1;
  KCD2S_CHECK(encode_world_time(time).empty());
  WorldTime decoded;
  KCD2S_CHECK(!decode_world_time({kWorldTimeTag, 1, 0, 0}, decoded));
}
