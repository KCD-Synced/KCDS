#include <algorithm>
#include <memory>
#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s;

namespace {
struct VisibilityWorld {
  replication::SchemaRegistry schema;
  std::unique_ptr<integration::Server> server;
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> linksA,
      linksB;
  std::unique_ptr<integration::ClientPeer> a, b;
  core::EntityId avatarA, avatarB, entity;
  core::PeerId peerA, peerB;
  unsigned tick = 0;
  VisibilityWorld() {
    schema.define("VisibleHuman").field("position", replication::FieldKind::Vec3);
    server = std::make_unique<integration::Server>(schema);
    auto& world = server->world();
    const auto type = schema.get("VisibleHuman")->id;
    avatarA = world.spawn(type);
    avatarB = world.spawn(type);
    entity = world.spawn(type);
    world.write(entity, 0, core::Vec3{10, 0, 0});
    linksA = transport::make_sim_pair({}, {});
    linksB = transport::make_sim_pair({}, {});
    peerA = server->add_client(linksA.first.get(), avatarA, 100);
    peerB = server->add_client(linksB.first.get(), avatarB, 100);
    server->set_owner(avatarA, peerA);
    server->set_owner(avatarB, peerB);
    a = std::make_unique<integration::ClientPeer>(schema, linksA.second.get());
    b = std::make_unique<integration::ClientPeer>(schema, linksB.second.get());
  }
  void advance() {
    for (unsigned count = 0; count < 8; count++) {
      const double now = ++tick * 0.05;
      server->world().set_tick(tick);
      server->tick(now);
      a->tick(now);
      b->tick(now);
    }
  }
};
}  // namespace

KCD2S_TEST(visibility, numbered_worlds_and_global_are_symmetric) {
  VisibilityWorld f;
  KCD2S_CHECK(f.server->world().slot(f.entity)->dimension == replication::kGlobalWorld);
  f.server->set_dimension(f.avatarA, 1);
  f.server->set_dimension(f.avatarB, 2);
  f.server->set_dimension(f.entity, 1);
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.entity));
  KCD2S_CHECK(!f.b->mirror().get(f.entity));
  KCD2S_CHECK(!f.a->mirror().get(f.avatarB));
  f.server->set_dimension(f.entity, 2);
  f.advance();
  KCD2S_CHECK(!f.a->mirror().get(f.entity));
  KCD2S_CHECK(f.b->mirror().get(f.entity));
  f.server->set_dimension(f.avatarA, replication::kGlobalWorld);
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.entity));
  KCD2S_CHECK(f.a->mirror().get(f.avatarB));
  f.server->set_dimension(f.entity, replication::kGlobalWorld);
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.entity));
  KCD2S_CHECK(f.b->mirror().get(f.entity));
}

KCD2S_TEST(visibility, maps_never_meet_not_even_through_their_global_worlds) {
  VisibilityWorld f;
  const std::uint32_t second_global = replication::on_map(1, replication::kGlobalWorld);
  // A on the first map, B and the entity on the second, both in its global world.
  f.server->set_dimension(f.avatarB, second_global);
  f.server->set_dimension(f.entity, second_global);
  f.advance();
  KCD2S_CHECK(!f.a->mirror().get(f.entity));
  KCD2S_CHECK(!f.a->mirror().get(f.avatarB));
  KCD2S_CHECK(f.b->mirror().get(f.entity));
  // A numbered world of the second map meets its global world, never the first map's.
  f.server->set_dimension(f.avatarB, replication::on_map(1, 7));
  f.advance();
  KCD2S_CHECK(f.b->mirror().get(f.entity));
  KCD2S_CHECK(!f.b->mirror().get(f.avatarA));
  // A goes to the second map: everything there comes in.
  f.server->set_dimension(f.avatarA, second_global);
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.entity));
  KCD2S_CHECK(f.a->mirror().get(f.avatarB));
}

KCD2S_TEST(visibility, the_first_map_keeps_the_one_map_values) {
  KCD2S_CHECK_EQ(replication::on_map(0, replication::kGlobalWorld), replication::kGlobalWorld);
  KCD2S_CHECK_EQ(replication::on_map(0, 5u), 5u);
  KCD2S_CHECK_EQ(replication::map_of(replication::kGlobalWorld), 0u);
  KCD2S_CHECK_EQ(replication::map_of(5u), 0u);
  KCD2S_CHECK_EQ(replication::map_of(replication::on_map(2, 5u)), 2u);
  KCD2S_CHECK(replication::worlds_meet(replication::kGlobalWorld, 5u));
  KCD2S_CHECK(!replication::worlds_meet(4u, 5u));
  KCD2S_CHECK(!replication::worlds_meet(replication::kGlobalWorld,
                                        replication::on_map(1, replication::kGlobalWorld)));
  KCD2S_CHECK(replication::worlds_meet(replication::on_map(1, replication::kGlobalWorld),
                                       replication::on_map(1, 9u)));
}

KCD2S_TEST(visibility, private_view_captures_connection_and_preserves_range_and_world_checks) {
  VisibilityWorld f;
  f.advance();
  KCD2S_CHECK(f.server->set_visible_to(f.entity, f.avatarA));
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.entity));
  KCD2S_CHECK(!f.b->mirror().get(f.entity));
  f.server->set_dimension(f.avatarA, 1);
  f.server->set_dimension(f.entity, 2);
  f.advance();
  KCD2S_CHECK(!f.a->mirror().get(f.entity));
  f.server->set_dimension(f.entity, 1);
  f.server->world().write(f.entity, 0, core::Vec3{1000, 0, 0});
  f.advance();
  KCD2S_CHECK(!f.a->mirror().get(f.entity));
  f.server->world().write(f.entity, 0, core::Vec3{10, 0, 0});
  f.server->set_owner(f.avatarA, f.peerB);
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.entity));
  KCD2S_CHECK(!f.b->mirror().get(f.entity));
  KCD2S_CHECK(f.server->set_visible_to(f.entity));
  f.advance();
  KCD2S_CHECK(f.b->mirror().get(f.entity));
}

KCD2S_TEST(visibility, ownership_election_skips_candidates_outside_entity_scope) {
  // A departing owner's entity must only be re-delegated to a client that can
  // actually see it. A candidate separated by a numbered dimension or hidden by
  // private visibility cannot inherit ownership; the entity falls back to the server.
  {
    VisibilityWorld f;
    f.server->set_owner(f.entity, f.peerA);
    f.server->set_dimension(f.avatarB, 1);
    f.server->set_dimension(f.entity, 2);
    f.server->remove_client(f.peerA);
    KCD2S_CHECK(f.server->world().slot(f.entity)->owner == core::PeerId{0});
  }
  {
    VisibilityWorld f;
    f.server->set_owner(f.entity, f.peerA);
    KCD2S_CHECK(f.server->set_visible_to(f.entity, f.avatarA));  // private to peerA only
    f.server->remove_client(f.peerA);
    KCD2S_CHECK(f.server->world().slot(f.entity)->owner == core::PeerId{0});
  }
  {
    VisibilityWorld f;
    f.server->set_owner(f.entity, f.peerA);
    // peerB shares the global world, is within range and unrestricted: a valid heir.
    f.server->remove_client(f.peerA);
    KCD2S_CHECK(f.server->world().slot(f.entity)->owner == f.peerB);
  }
}

KCD2S_TEST(visibility, explicit_set_owner_notification_honours_live_scope) {
  // set_owner records authority unconditionally, but its SetOwner control message
  // carries the entity id/type and must not reach a peer that cannot currently see
  // the target. owns() reflects exactly that message, so it is the direct evidence
  // of the (non-)leak; slot->owner proves the authority itself is still assigned.
  {
    // Numbered-world separation: the entity lives in world 2, peerB's avatar in 1.
    VisibilityWorld f;
    f.server->set_dimension(f.avatarB, 1);
    f.server->set_dimension(f.entity, 2);
    f.server->set_owner(f.entity, f.peerB);
    f.advance();
    KCD2S_CHECK(f.server->world().slot(f.entity)->owner == f.peerB);  // authority kept
    KCD2S_CHECK(!f.b->owns(f.entity));                                // id not disclosed
    // Bring the entity into peerB's world; a fresh assignment now notifies normally.
    f.server->set_dimension(f.entity, 1);
    f.server->set_owner(f.entity, f.peerB);
    f.advance();
    KCD2S_CHECK(f.b->owns(f.entity));
  }
  {
    // Private-visibility restriction: the entity is visible only to peerA.
    VisibilityWorld f;
    KCD2S_CHECK(f.server->set_visible_to(f.entity, f.avatarA));
    f.server->set_owner(f.entity, f.peerB);
    f.advance();
    KCD2S_CHECK(f.server->world().slot(f.entity)->owner == f.peerB);
    KCD2S_CHECK(!f.b->owns(f.entity));
    // peerA may see it, so assigning ownership to peerA notifies as usual.
    f.server->set_owner(f.entity, f.peerA);
    f.advance();
    KCD2S_CHECK(f.a->owns(f.entity));
  }
  {
    // Live re-evaluation under a world transition: an entity that starts in scope is
    // moved into a numbered world and the viewer into a different one, then ownership
    // is assigned. The gate reads the current slots, so the notification is withheld
    // while separated and delivered once the worlds rejoin -- no stale pre-transition
    // bookkeeping lets the id through.
    VisibilityWorld f;
    f.advance();  // both start in the global world
    f.server->set_dimension(f.avatarA, 3);
    f.server->set_dimension(f.entity, 4);
    f.server->set_owner(f.entity, f.peerA);
    f.advance();
    KCD2S_CHECK(f.server->world().slot(f.entity)->owner == f.peerA);
    KCD2S_CHECK(!f.a->owns(f.entity));     // separated: withheld
    f.server->set_dimension(f.entity, 3);  // rejoin peerA's world
    f.server->set_owner(f.entity, f.peerA);
    f.advance();
    KCD2S_CHECK(f.a->owns(f.entity));
  }
}

KCD2S_TEST(visibility,
           stale_ids_and_disconnected_private_connections_never_reveal_to_replacements) {
  VisibilityWorld f;
  KCD2S_CHECK(f.server->set_visible_to(f.entity, f.avatarA));
  f.server->remove_client(f.peerA);
  f.advance();
  KCD2S_CHECK(!f.b->mirror().get(f.entity));
  KCD2S_CHECK(!f.server->set_visible_to(f.entity, f.entity));
  const auto previous = f.entity;
  f.server->despawn(previous);
  f.entity = f.server->world().spawn(f.schema.get("VisibleHuman")->id);
  KCD2S_CHECK(f.entity != previous);
  KCD2S_CHECK(!f.server->set_dimension(previous, 1));
  KCD2S_CHECK(!f.server->set_visible_to(previous, f.avatarB));
  KCD2S_CHECK(f.server->world().slot(f.entity)->visibleTo == 0);
  KCD2S_CHECK(f.server->world().slot(f.entity)->dimension == replication::kGlobalWorld);
  f.advance();
  KCD2S_CHECK(f.b->mirror().get(f.entity));
}

KCD2S_TEST(visibility, private_restriction_on_an_avatar_never_hides_it_from_its_own_client) {
  VisibilityWorld f;
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.avatarA));
  KCD2S_CHECK(f.b->mirror().get(f.avatarA));

  // Restricting avatar A to connection A hides it from B; A keeps its own avatar.
  KCD2S_CHECK(f.server->set_visible_to(f.avatarA, f.avatarA));
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.avatarA));
  KCD2S_CHECK(f.a->owns(f.avatarA));
  KCD2S_CHECK(!f.b->mirror().get(f.avatarA));

  // Restricting avatar A to connection B shows it to B only; A still keeps itself.
  KCD2S_CHECK(f.server->set_visible_to(f.avatarA, f.avatarB));
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.avatarA));
  KCD2S_CHECK(f.b->mirror().get(f.avatarA));
  const auto own_view = f.server->visible_entities(f.peerA);
  KCD2S_CHECK(std::find(own_view.begin(), own_view.end(), f.avatarA) != own_view.end());

  // A forced teleport still reaches the owning client of a restricted avatar.
  KCD2S_CHECK(f.server->set_visible_to(f.avatarA, f.avatarB));
  KCD2S_CHECK(f.server->force_transform(f.avatarA, core::Vec3{5, 0, 0}));
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.avatarA));

  KCD2S_CHECK(f.server->set_visible_to(f.avatarA));
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.avatarA));
  KCD2S_CHECK(f.b->mirror().get(f.avatarA));
}

KCD2S_TEST(visibility, avatars_in_different_numbered_worlds_hide_each_other_but_keep_themselves) {
  VisibilityWorld f;
  f.advance();
  f.server->set_dimension(f.avatarA, 1);
  f.server->set_dimension(f.avatarB, 2);
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.avatarA));
  KCD2S_CHECK(f.b->mirror().get(f.avatarB));
  KCD2S_CHECK(!f.a->mirror().get(f.avatarB));
  KCD2S_CHECK(!f.b->mirror().get(f.avatarA));

  f.server->set_dimension(f.avatarB, 1);
  f.advance();
  KCD2S_CHECK(f.a->mirror().get(f.avatarB));
  KCD2S_CHECK(f.b->mirror().get(f.avatarA));
}
