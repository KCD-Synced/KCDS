#include <algorithm>
#include <vector>

#include "kcd2s/testkit.hpp"
#include "kcd2s/voice/relay.hpp"

using kcd2s::core::EntityId;
using kcd2s::core::Vec3;
using namespace kcd2s::voice;

namespace {

bool hears(const std::vector<EntityId>& listeners, EntityId id) {
  return std::find(listeners.begin(), listeners.end(), id) != listeners.end();
}

Relay with_players(std::initializer_list<std::pair<EntityId, float>> players) {
  Relay relay;
  for (const auto& [id, x] : players) {
    relay.add_player(id);
    relay.set_position(id, Vec3{x, 0.0f, 0.0f}, 0);
  }
  return relay;
}

}  // namespace

KCD2S_TEST(voice_relay, each_tier_reaches_as_far_as_its_range) {
  Relay relay = with_players({{1, 0.0f}, {2, 1.5f}, {3, 8.0f}, {4, 25.0f}, {5, 40.0f}});
  std::vector<EntityId> out;
  relay.set_tier(1, Tier::Whisper);
  relay.route(1, 200, 1000, out);
  KCD2S_CHECK(hears(out, 2) && !hears(out, 3));
  relay.set_tier(1, Tier::Normal);
  relay.route(1, 200, 1001, out);
  KCD2S_CHECK(hears(out, 3) && !hears(out, 4));
  relay.set_tier(1, Tier::Shout);
  relay.route(1, 200, 1002, out);
  KCD2S_CHECK(hears(out, 4) && !hears(out, 5));
  KCD2S_CHECK(!hears(out, 1));
}

KCD2S_TEST(voice_relay, a_shout_longer_than_a_grid_cell_is_heard) {
  RelayConfig config;
  config.grid_cell_meters = 5.0f;
  Relay relay(config);
  relay.add_player(1);
  relay.add_player(2);
  relay.set_position(1, Vec3{0.0f, 0.0f, 0.0f}, 0);
  relay.set_position(2, Vec3{28.0f, 0.0f, 0.0f}, 0);
  relay.set_tier(1, Tier::Shout);
  std::vector<EntityId> out;
  relay.route(1, 200, 1000, out);
  KCD2S_CHECK(hears(out, 2));
}

KCD2S_TEST(voice_relay, height_counts_for_the_range) {
  Relay relay = with_players({{1, 0.0f}, {2, 0.0f}});
  relay.set_position(2, Vec3{0.0f, 0.0f, 20.0f}, 0);
  std::vector<EntityId> out;
  relay.route(1, 200, 1000, out);
  KCD2S_CHECK(!hears(out, 2));
}

KCD2S_TEST(voice_relay, other_worlds_mute_deaf_and_off_are_silent) {
  Relay relay = with_players({{1, 0.0f}, {2, 1.0f}, {3, 1.0f}, {4, 1.0f}});
  std::vector<EntityId> out;
  relay.set_position(2, Vec3{1.0f, 0.0f, 0.0f}, 7);
  relay.set_deaf(3, true);
  relay.route(1, 200, 1000, out);
  KCD2S_CHECK(!hears(out, 2) && !hears(out, 3) && hears(out, 4));

  relay.set_muted(1, true);
  relay.route(1, 200, 1001, out);
  KCD2S_CHECK(out.empty());
  relay.set_muted(1, false);

  relay.set_enabled(false);
  relay.route(1, 200, 1002, out);
  KCD2S_CHECK(out.empty());
}

KCD2S_TEST(voice_relay, a_listener_mute_touches_only_that_listener) {
  Relay relay = with_players({{1, 0.0f}, {2, 1.0f}, {3, 1.0f}});
  std::vector<EntityId> out;
  relay.set_listener_mute(2, 1, true);
  relay.route(1, 200, 1000, out);
  KCD2S_CHECK(!hears(out, 2) && hears(out, 3));
  relay.set_listener_mute(2, 1, false);
  relay.route(1, 200, 1001, out);
  KCD2S_CHECK(hears(out, 2));
}

KCD2S_TEST(voice_relay, a_listener_hears_at_most_eight_and_a_better_talker_takes_a_place) {
  Relay relay;
  relay.add_player(100);
  relay.set_position(100, Vec3{0.0f, 0.0f, 0.0f}, 0);
  for (EntityId id = 1; id <= 9; ++id) {
    relay.add_player(id);
    relay.set_position(id, Vec3{5.0f, 0.0f, 0.0f}, 0);
  }
  std::vector<EntityId> out;
  for (EntityId id = 1; id <= 8; ++id) {
    relay.route(id, 50, 1000, out);
    KCD2S_CHECK(hears(out, 100));
  }
  // Nearly the same score does not push anyone out, so places do not flip.
  relay.route(9, 55, 1001, out);
  KCD2S_CHECK(!hears(out, 100));
  // A clearly louder talker does.
  relay.route(9, 255, 1002, out);
  KCD2S_CHECK(hears(out, 100));
}

KCD2S_TEST(voice_relay, a_silent_talker_gives_up_the_place) {
  SpeakerSelector selector;
  RelayConfig config;
  for (EntityId id = 1; id <= kMaxSpeakers; ++id) {
    KCD2S_CHECK(selector.admit(id, 0.9f, 1000, config));
  }
  KCD2S_CHECK(!selector.admit(50, 0.1f, 1100, config));
  KCD2S_CHECK_EQ(selector.active(1100, config), kMaxSpeakers);
  KCD2S_CHECK(selector.admit(50, 0.1f, 1000 + config.speaker_timeout_ms + 1, config));
}

KCD2S_TEST(voice_relay, a_player_who_left_is_gone_everywhere) {
  Relay relay = with_players({{1, 0.0f}, {2, 1.0f}});
  std::vector<EntityId> out;
  relay.route(1, 200, 1000, out);
  KCD2S_CHECK(hears(out, 2));
  relay.remove_player(2);
  relay.route(1, 200, 1001, out);
  KCD2S_CHECK(out.empty());
  relay.route(2, 200, 1002, out);
  KCD2S_CHECK(out.empty());
  KCD2S_CHECK(!relay.has_player(2));
}

KCD2S_TEST(voice_relay, a_player_with_no_position_yet_sends_nothing) {
  Relay relay;
  relay.add_player(1);
  std::vector<EntityId> out;
  relay.route(1, 200, 1000, out);
  KCD2S_CHECK(out.empty());
}
