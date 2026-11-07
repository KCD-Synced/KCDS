#include "kcd2s/integration/world_weather.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

namespace {

WorldWeather sample() {
  WorldWeather weather;
  weather.preset = "stormy_rain";
  weather.previous = "cloudless_sunny";
  weather.seconds = 600.0f;
  weather.remaining = 120.5f;
  weather.rainStrength = 0.75f;
  weather.rainLevel = 0.5f;
  weather.windX = 3.0f;
  weather.windY = -2.5f;
  weather.windZ = 0.0f;
  weather.snowAmount = 4.5f;
  weather.snowFrost = 0.25f;
  weather.snowFreezing = 0.5f;
  weather.snowFlakes = 300;
  return weather;
}

}  // namespace

KCD2S_TEST(world_weather, round_trips) {
  const WorldWeather weather = sample();
  WorldWeather decoded;
  KCD2S_CHECK(decode_world_weather(encode_world_weather(weather), decoded));
  KCD2S_CHECK(decoded == weather);
}

KCD2S_TEST(world_weather, refuses_out_of_range) {
  WorldWeather weather = sample();
  weather.preset.clear();
  KCD2S_CHECK(encode_world_weather(weather).empty());
  weather = sample();
  weather.preset.assign(kMaxWeatherPresetLength + 1, 'x');
  KCD2S_CHECK(encode_world_weather(weather).empty());
  weather = sample();
  weather.remaining = -1.0f;
  KCD2S_CHECK(encode_world_weather(weather).empty());
  weather = sample();
  weather.rainStrength = 1.5f;
  KCD2S_CHECK(encode_world_weather(weather).empty());
  weather = sample();
  weather.windX = kMaxWeatherWind + 1.0f;
  KCD2S_CHECK(encode_world_weather(weather).empty());
  weather = sample();
  weather.snowAmount = kMaxSnowAmount + 1.0f;
  KCD2S_CHECK(encode_world_weather(weather).empty());
  weather = sample();
  weather.snowFrost = -0.5f;
  KCD2S_CHECK(encode_world_weather(weather).empty());
  weather = sample();
  weather.snowFlakes = kMaxSnowFlakes + 1;
  KCD2S_CHECK(encode_world_weather(weather).empty());
}

KCD2S_TEST(world_weather, refuses_a_short_message) {
  std::vector<std::uint8_t> bytes = encode_world_weather(sample());
  bytes.resize(bytes.size() - 3);
  WorldWeather decoded;
  KCD2S_CHECK(!decode_world_weather(bytes, decoded));
  KCD2S_CHECK(!decode_world_weather({kWorldWeatherTag, 0, 0}, decoded));
}
