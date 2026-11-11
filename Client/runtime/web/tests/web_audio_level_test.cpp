#include "../web_audio_level.hpp"

#include <cmath>
#include <limits>

#include "kcd2s/testkit.hpp"

namespace audio = kcds::runtime::web::audio;

namespace {

bool near(float a, float b) {
  return std::fabs(a - b) < 0.0001f;
}

}  // namespace

KCD2S_TEST(web_audio_level, the_players_setting_times_the_servers_share) {
  KCD2S_CHECK(near(audio::level(1.0f, 100.0f, false), 1.0f));
  KCD2S_CHECK(near(audio::level(0.5f, 100.0f, false), 0.5f));
  KCD2S_CHECK(near(audio::level(0.5f, 40.0f, false), 0.2f));
  KCD2S_CHECK(near(audio::level(1.0f, 0.0f, false), 0.0f));
}

KCD2S_TEST(web_audio_level, muted_by_the_server_is_silent_whatever_the_volumes) {
  KCD2S_CHECK(near(audio::level(1.0f, 100.0f, true), 0.0f));
}

KCD2S_TEST(web_audio_level, values_out_of_range_are_held_in_it) {
  KCD2S_CHECK(near(audio::level(3.0f, 250.0f, false), 1.0f));
  KCD2S_CHECK(near(audio::level(-1.0f, 100.0f, false), 0.0f));
  KCD2S_CHECK(near(audio::level(std::numeric_limits<float>::quiet_NaN(), 50.0f, false), 0.5f));
}
