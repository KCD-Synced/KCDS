#include "kcd2s/integration/session.hpp"

#include <string>
#include <variant>
#include <vector>

#include "kcd2s/replication/world.hpp"
#include "kcd2s/testkit.hpp"

using kcd2s::core::PeerId;
using kcd2s::integration::Session;
using kcd2s::replication::Vec3;

KCD2S_TEST(session, players_visible_to_each_other) {
  Session s;
  const PeerId a = s.join("alice");
  const PeerId b = s.join("bob");
  const PeerId c = s.join("carol");
  s.tick(12);

  // Every client's mirror holds all three player avatars (incl. its own).
  KCD2S_CHECK_EQ(s.players_visible_to(a), std::size_t{3});
  KCD2S_CHECK_EQ(s.players_visible_to(b), std::size_t{3});
  KCD2S_CHECK_EQ(s.players_visible_to(c), std::size_t{3});
}

KCD2S_TEST(session, roster_shared_state_converges) {
  Session s;
  const PeerId a = s.join("alice");
  const PeerId b = s.join("bob");
  s.tick(12);

  KCD2S_CHECK_EQ(s.roster_size(a), std::size_t{2});
  KCD2S_CHECK_EQ(s.roster_size(b), std::size_t{2});
  KCD2S_CHECK(s.roster_has(a, a) && s.roster_has(a, b));
  KCD2S_CHECK(s.roster_has(b, a) && s.roster_has(b, b));
  // Each client saw its roster entries arrive (change callbacks fired with additions).
  KCD2S_CHECK(s.roster_changes(a).size() >= std::size_t{2});
  bool add_seen = false;
  for (const auto& ch : s.roster_changes(a))
    if (ch.size() >= 2 && ch.back() == '+')
      add_seen = true;
  KCD2S_CHECK(add_seen);
}

KCD2S_TEST(session, chat_reaches_all_clients) {
  Session s;
  const PeerId a = s.join("alice");
  const PeerId b = s.join("bob");
  const PeerId c = s.join("carol");
  s.tick(8);

  s.say(a, "hello world");
  s.tick(6);  // client -> server -> all clients

  for (PeerId p : {a, b, c}) {
    bool saw = false;
    for (const auto& line : s.chat_log(p))
      if (line == "alice: hello world")
        saw = true;
    KCD2S_CHECK(saw);
  }
}

KCD2S_TEST(session, owner_move_replicates_to_others) {
  Session s;
  const PeerId a = s.join("alice");
  const PeerId b = s.join("bob");
  s.tick(10);  // let A learn ownership of its avatar (SET_OWNER)

  s.move(a, Vec3{500.0f, 0.0f, 25.0f});
  s.tick(10);

  // Server accepted the owner write and replicated it to B's mirror.
  const auto* a_on_b = s.mirror(b).get(s.avatar_of(a));
  KCD2S_CHECK(a_on_b != nullptr);
  KCD2S_CHECK((std::get<Vec3>(a_on_b->fields[0]) == Vec3{500.0f, 0.0f, 25.0f}));
}

KCD2S_TEST(session, disconnect_cleans_up_avatar_and_roster) {
  Session s;
  const PeerId a = s.join("alice");
  const PeerId b = s.join("bob");
  const PeerId c = s.join("carol");
  s.tick(12);
  KCD2S_CHECK_EQ(s.players_visible_to(a), std::size_t{3});
  KCD2S_CHECK_EQ(s.roster_size(a), std::size_t{3});

  const kcd2s::core::EntityId bob_avatar = s.avatar_of(b);
  s.leave(b);
  s.tick(12);

  // Remaining clients drop bob's avatar and roster entry.
  KCD2S_CHECK_EQ(s.member_count(), std::size_t{2});
  KCD2S_CHECK_EQ(s.players_visible_to(a), std::size_t{2});
  KCD2S_CHECK_EQ(s.players_visible_to(c), std::size_t{2});
  KCD2S_CHECK(s.mirror(a).get(bob_avatar) == nullptr);  // avatar DESTRUCTed
  KCD2S_CHECK_EQ(s.roster_size(a), std::size_t{2});
  KCD2S_CHECK(!s.roster_has(a, b));  // roster entry removed
  KCD2S_CHECK(!s.roster_has(c, b));
  // A saw the removal callback (tombstone).
  bool removal_seen = false;
  for (const auto& ch : s.roster_changes(a))
    if (ch.size() >= 2 && ch.back() == '-')
      removal_seen = true;
  KCD2S_CHECK(removal_seen);
}
