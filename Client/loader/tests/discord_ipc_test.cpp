#include "discord/discord_ipc.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcds::loader::discord;

// Discord's IPC frames the loader sends and reads.
KCD2S_TEST(discord_ipc, frames) {
  const auto bytes = frame(1, "{}");
  KCD2S_CHECK(bytes.size() == 10 && bytes[0] == 1 && bytes[4] == 2 && bytes[8] == '{');
  KCD2S_CHECK(handshake_json("123") == "{\"v\":1,\"client_id\":\"123\"}");
  KCD2S_CHECK(set_activity_json(42, "", 7) ==
              "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":null},\"nonce\":\"7\"}");
  KCD2S_CHECK(set_activity_json(42, "{\"details\":\"x\"}", 8) ==
              "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":{\"details\":\"x\"}},"
              "\"nonce\":\"8\"}");
  KCD2S_CHECK(ready_user_id("{\"cmd\":\"DISPATCH\",\"data\":{\"v\":1,\"user\":{\"id\":"
                            "\"80351110224678912\",\"username\":\"x\"}},\"evt\":\"READY\"}") ==
              "80351110224678912");
  KCD2S_CHECK(ready_user_id("{\"code\":4000,\"message\":\"Invalid Client ID\"}").empty());
}

// AUTHORIZE: the code of Discord's answer, none when the player declined.
KCD2S_TEST(discord_ipc, authorize_frames) {
  KCD2S_CHECK(authorize_json("123", 4) ==
              "{\"cmd\":\"AUTHORIZE\",\"args\":{\"client_id\":\"123\",\"scopes\":"
              "[\"identify\"]},\"nonce\":\"auth4\"}");
  KCD2S_CHECK(authorize_code("{\"cmd\":\"AUTHORIZE\",\"data\":{\"code\":\"Abc123xyz\"},"
                             "\"evt\":null,\"nonce\":\"auth4\"}") == "Abc123xyz");
  KCD2S_CHECK(authorize_code("{\"cmd\":\"AUTHORIZE\",\"data\":{\"code\":5000,\"message\":"
                             "\"OAuth2 Error: access_denied\"},\"evt\":\"ERROR\"}")
                  .empty());
  KCD2S_CHECK(authorize_code("{\"cmd\":\"SET_ACTIVITY\",\"data\":{\"code\":\"x\"}}").empty());
  KCD2S_CHECK(is_authorize_answer("{\"cmd\":\"AUTHORIZE\",\"evt\":\"ERROR\"}"));
}

// When the activity goes to Discord: a change at most every 4 seconds, an unchanged one again
// after 30 seconds.
KCD2S_TEST(discord_ipc, publish_due) {
  KCD2S_CHECK(publish_due("a", "", 10.0, -4.0, 4.0, 30.0));
  KCD2S_CHECK(!publish_due("b", "a", 12.0, 10.0, 4.0, 30.0));
  KCD2S_CHECK(publish_due("b", "a", 14.0, 10.0, 4.0, 30.0));
  KCD2S_CHECK(!publish_due("a", "a", 20.0, 10.0, 4.0, 30.0));
  KCD2S_CHECK(publish_due("a", "a", 40.0, 10.0, 4.0, 30.0));
}
