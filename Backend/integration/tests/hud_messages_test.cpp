#include "kcd2s/integration/server_name.hpp"
#include "kcd2s/integration/voice_messages.hpp"

#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// The watermark's SERVER_NAME and the voice indicator's byte of VOICE_STATE.

KCD2S_TEST(hud_messages, server_name_round_trips) {
  std::string decoded;
  KCD2S_CHECK(decode_server_name(encode_server_name("My Server \xe2\x80\x94 Trosky"), decoded));
  KCD2S_CHECK(decoded == "My Server \xe2\x80\x94 Trosky");
  // At most 64 bytes go out; an empty name or a wrong length is refused.
  KCD2S_CHECK(decode_server_name(encode_server_name(std::string(80, 'x')), decoded));
  KCD2S_CHECK(decoded.size() == kMaxServerNameLength);
  KCD2S_CHECK(!decode_server_name(encode_server_name(""), decoded));
  auto bytes = encode_server_name("Trosky");
  bytes.push_back('!');
  KCD2S_CHECK(!decode_server_name(bytes, decoded));
}

KCD2S_TEST(hud_messages, voice_state_carries_the_indicator) {
  VoiceState state;
  state.tier = 2;
  state.indicator = false;
  VoiceState decoded;
  KCD2S_CHECK(decode_voice_state(encode_voice_state(state), decoded) && decoded == state);
  KCD2S_CHECK(!decoded.indicator);
  // An older server's state, without the byte, shows the indicator.
  auto older = encode_voice_state(state);
  older.pop_back();
  KCD2S_CHECK(decode_voice_state(older, decoded));
  KCD2S_CHECK(decoded.indicator && decoded.tier == 2);
}
