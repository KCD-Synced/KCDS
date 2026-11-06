#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/camera_shot.hpp"
#include "kcd2s/integration/weapon_hits.hpp"
#include "kcd2s/integration/world_ray.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s;
using namespace kcd2s::integration;

namespace {

WorldRayRequest sample_ray() {
  WorldRayRequest ray;
  ray.request = 4242;
  ray.from[0] = 1200.5f;
  ray.from[1] = -330.25f;
  ray.from[2] = 90.0f;
  ray.to[0] = 1200.5f;
  ray.to[1] = -330.25f;
  ray.to[2] = 10.0f;
  ray.mode = RayMode::Anything;
  return ray;
}

WorldRayReply sample_hit() {
  WorldRayReply reply;
  reply.request = 4242;
  reply.status = RayStatus::Hit;
  WorldRayHit hit;
  hit.position[0] = 1200.5f;
  hit.position[1] = -330.25f;
  hit.position[2] = 88.0f;
  hit.normal[2] = 1.0f;
  hit.distance = 2.0f;
  hit.terrain = true;
  hit.entity = 17;
  hit.surface = "mat_grass";
  // What it met and the level object a level edit finds it by.
  hit.kind = RayHitKind::Brush;
  hit.model = "objects/natural/rocks/rock_a.cgf";
  hit.object = RayObject::Brush;
  hit.object_at[0] = 1201.0f;
  hit.object_at[1] = -330.0f;
  hit.object_at[2] = 87.5f;
  hit.object_rotation[2] = 0.70710677f;
  hit.object_rotation[3] = 0.70710677f;
  hit.object_scale[2] = 1.5f;
  reply.hits.push_back(hit);
  return reply;
}

CameraShot sample_shot() {
  CameraShot shot;
  shot.position[0] = 10.0f;
  shot.position[1] = 20.0f;
  shot.position[2] = 30.0f;
  shot.aim = CameraAim::LookAt;
  shot.target[0] = 15.0f;
  shot.target[1] = 20.0f;
  shot.target[2] = 30.0f;
  shot.roll = -12.5f;
  shot.fov = 55.0f;
  shot.seconds = 2.5f;
  return shot;
}

// A server, one player's client on a loopback link, and the client's world made ready.
struct Session {
  replication::SchemaRegistry schema;
  std::unique_ptr<Server> server;
  std::unique_ptr<ClientPeer> client;
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
  core::EntityId avatar = 0;
  double now = 0;

  Session() {
    auto type = schema.define("Player")
                    .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                           replication::Authority::Owner)
                    .id();
    server = std::make_unique<Server>(schema);
    server->world().set_tick(1);
    avatar = server->world().spawn(type);
    server->world().write(avatar, 0, core::Vec3{});
    links = transport::make_loopback_pair();
    auto peer = server->add_client(links.first.get(), avatar, 10000);
    server->configure_world(peer, WorldConfig{});
    server->set_owner(avatar, peer);
    client = std::make_unique<ClientPeer>(schema, links.second.get());
    pump(8);
  }

  void pump(int count) {
    for (int i = 0; i < count; ++i) {
      now += 1.0 / 60;
      server->world().set_tick(server->world().tick() + 1);
      server->tick(now);
      client->tick(now);
    }
  }

  bool ready() {
    const bool marked = client->mark_world_ready(1);
    pump(8);
    return marked;
  }
};

}  // namespace

KCD2S_TEST(world_ray, request_round_trips_and_refuses_bad_rays) {
  WorldRayRequest decoded;
  KCD2S_CHECK(decode_world_raycast(encode_world_raycast(sample_ray()), decoded));
  KCD2S_CHECK(decoded == sample_ray());

  WorldRayRequest empty = sample_ray();
  empty.to[2] = empty.from[2];
  KCD2S_CHECK(encode_world_raycast(empty).empty());
  WorldRayRequest long_ray = sample_ray();
  long_ray.to[2] = long_ray.from[2] - kMaxRayLength - 1.0f;
  KCD2S_CHECK(encode_world_raycast(long_ray).empty());
  WorldRayRequest far = sample_ray();
  far.from[0] = far.to[0] = kMaxRayCoordinate * 2.0f;
  KCD2S_CHECK(encode_world_raycast(far).empty());

  auto message = encode_world_raycast(sample_ray());
  message[message.size() - 2] = kRayModeCount;  // the mode byte before the hit count
  KCD2S_CHECK(!decode_world_raycast(message, decoded));
  message = encode_world_raycast(sample_ray());
  message.push_back(0);
  KCD2S_CHECK(!decode_world_raycast(message, decoded));
  message = encode_world_raycast(sample_ray());
  message.pop_back();
  KCD2S_CHECK(!decode_world_raycast(message, decoded));
}

KCD2S_TEST(world_ray, reply_round_trips_hit_miss_and_unavailable) {
  WorldRayReply decoded;
  KCD2S_CHECK(decode_world_raycast_reply(encode_world_raycast_reply(sample_hit()), decoded));
  KCD2S_CHECK(decoded == sample_hit());

  WorldRayReply miss;
  miss.request = 9;
  KCD2S_CHECK(decode_world_raycast_reply(encode_world_raycast_reply(miss), decoded));
  KCD2S_CHECK(decoded == miss);
  KCD2S_CHECK(encode_world_raycast_reply(miss).size() == 7);

  WorldRayReply unavailable;
  unavailable.status = RayStatus::Unavailable;
  KCD2S_CHECK(decode_world_raycast_reply(encode_world_raycast_reply(unavailable), decoded));
  KCD2S_CHECK(decoded.status == RayStatus::Unavailable);

  WorldRayReply bad = sample_hit();
  bad.hits[0].distance = std::nanf("");
  KCD2S_CHECK(encode_world_raycast_reply(bad).empty());
  bad = sample_hit();
  bad.hits.push_back(bad.hits[0]);  // two hits for the first-hit query
  KCD2S_CHECK(encode_world_raycast_reply(bad).empty());
  bad = sample_hit();
  bad.hits[0].surface = std::string(kMaxSurfaceName + 1, 'a');
  KCD2S_CHECK(encode_world_raycast_reply(bad).empty());
  bad = sample_hit();
  bad.hits[0].model = std::string(kMaxRayText + 1, 'a');
  KCD2S_CHECK(encode_world_raycast_reply(bad).empty());
  bad = sample_hit();
  bad.hits[0].object_scale[0] = std::nanf("");
  KCD2S_CHECK(encode_world_raycast_reply(bad).empty());
  bad = sample_hit();
  bad.hits[0].kind = RayHitKind(9);
  KCD2S_CHECK(encode_world_raycast_reply(bad).empty());
  bad = sample_hit();
  bad.hits.clear();
  KCD2S_CHECK(encode_world_raycast_reply(bad).empty());
  auto message = encode_world_raycast_reply(sample_hit());
  message.resize(10);
  KCD2S_CHECK(!decode_world_raycast_reply(message, decoded));
}

KCD2S_TEST(world_ray, every_hit_and_bodies_round_trip) {
  WorldRayRequest all = sample_ray();
  all.query = RayQuery::All;
  all.max_hits = kMaxRayHits;
  WorldRayRequest decoded_request;
  KCD2S_CHECK(decode_world_raycast(encode_world_raycast(all), decoded_request));
  KCD2S_CHECK(decoded_request == all);
  all.max_hits = kMaxRayHits + 1;
  KCD2S_CHECK(encode_world_raycast(all).empty());
  WorldRayRequest first = sample_ray();
  first.max_hits = 2;
  KCD2S_CHECK(encode_world_raycast(first).empty());

  WorldRayReply hits = sample_hit();
  hits.query = RayQuery::All;
  WorldRayHit body = hits.hits[0];
  body.terrain = false;
  body.surface.clear();
  body.distance = 1.0f;
  hits.hits.insert(hits.hits.begin(), body);
  WorldRayReply decoded;
  KCD2S_CHECK(decode_world_raycast_reply(encode_world_raycast_reply(hits), decoded));
  KCD2S_CHECK(decoded == hits);

  WorldRayRequest around;
  around.request = 77;
  around.query = RayQuery::Bodies;
  around.center[0] = 10.0f;
  around.radius = 25.0f;
  KCD2S_CHECK(decode_world_raycast(encode_world_raycast(around), decoded_request));
  KCD2S_CHECK(decoded_request == around);
  around.radius = kMaxBodyRadius + 1.0f;
  KCD2S_CHECK(encode_world_raycast(around).empty());
  around.radius = 0.0f;
  KCD2S_CHECK(encode_world_raycast(around).empty());

  WorldRayReply bodies;
  bodies.request = 77;
  bodies.query = RayQuery::Bodies;
  bodies.status = RayStatus::Hit;
  WorldBody found;
  found.entity = 5;
  found.position[0] = 12.0f;
  bodies.bodies.assign(kMaxBodiesInRadius, found);
  KCD2S_CHECK(decode_world_raycast_reply(encode_world_raycast_reply(bodies), decoded));
  KCD2S_CHECK(decoded == bodies);
  bodies.bodies.push_back(found);
  KCD2S_CHECK(encode_world_raycast_reply(bodies).empty());
}

// the level's entities in a radius, answered as hits.
KCD2S_TEST(world_raycast, level_entities_round_trip_with_their_bounds) {
  WorldRayRequest entities;
  entities.request = 91;
  entities.query = RayQuery::Entities;
  entities.center[1] = 4.0f;
  entities.radius = 10.0f;
  entities.max_hits = 8;
  WorldRayRequest decoded_request;
  KCD2S_CHECK(decode_world_raycast(encode_world_raycast(entities), decoded_request));
  KCD2S_CHECK(decoded_request == entities);
  entities.radius = kMaxEntityRadius + 1.0f;
  KCD2S_CHECK(encode_world_raycast(entities).empty());
  entities.radius = 10.0f;
  entities.max_hits = kMaxLevelEntities + 1;
  KCD2S_CHECK(encode_world_raycast(entities).empty());

  WorldRayReply reply;
  reply.request = 91;
  reply.query = RayQuery::Entities;
  reply.status = RayStatus::Hit;
  WorldRayHit rack;
  rack.kind = RayHitKind::Entity;
  rack.position[0] = 2.0f;
  rack.normal[2] = 1.0f;
  rack.distance = 2.0f;
  rack.guid = "00ab00cd00ef0012";
  rack.name = "sausage_rack_1";
  rack.entity_class = "PickableItem";
  rack.object = RayObject::Entity;
  rack.object_at[0] = 2.0f;
  reply.hits.assign(kMaxLevelEntities, rack);
  WorldRayReply decoded;
  KCD2S_CHECK(decode_world_raycast_reply(encode_world_raycast_reply(reply), decoded));
  KCD2S_CHECK(decoded == reply);
  reply.hits.push_back(rack);
  KCD2S_CHECK(encode_world_raycast_reply(reply).empty());
}

KCD2S_TEST(camera_shot, round_trips_both_aims_and_refuses_bad_shots) {
  CameraShot decoded;
  KCD2S_CHECK(decode_camera_shot(encode_camera_shot(sample_shot()), decoded));
  KCD2S_CHECK(decoded == sample_shot());

  CameraShot turned = sample_shot();
  turned.aim = CameraAim::Rotation;
  turned.target[0] = turned.target[1] = turned.target[2] = 0.0f;
  turned.roll = 0.0f;  // a rotation carries its own
  turned.rotation[2] = 0.7071f;
  turned.rotation[3] = 0.7071f;
  turned.fov = 0.0f;
  turned.seconds = 0.0f;
  KCD2S_CHECK(decode_camera_shot(encode_camera_shot(turned), decoded));
  KCD2S_CHECK(decoded == turned);

  // One speed throughout; a message without the ease byte is eased.
  CameraShot linear = sample_shot();
  linear.ease = false;
  KCD2S_CHECK(decode_camera_shot(encode_camera_shot(linear), decoded));
  KCD2S_CHECK(decoded == linear && !decoded.ease);
  auto without_ease = encode_camera_shot(linear);
  without_ease.pop_back();
  KCD2S_CHECK(decode_camera_shot(without_ease, decoded));
  KCD2S_CHECK(decoded.ease);
  auto bad_ease = encode_camera_shot(linear);
  bad_ease.back() = 2;
  KCD2S_CHECK(!decode_camera_shot(bad_ease, decoded));

  CameraShot bad = sample_shot();
  bad.target[0] = bad.position[0];
  KCD2S_CHECK(encode_camera_shot(bad).empty());
  bad = sample_shot();
  bad.roll = kMaxCameraRoll + 1.0f;
  KCD2S_CHECK(encode_camera_shot(bad).empty());
  bad = sample_shot();
  bad.fov = 2.0f;
  KCD2S_CHECK(encode_camera_shot(bad).empty());
  bad = sample_shot();
  bad.seconds = kMaxCameraSeconds + 1.0f;
  KCD2S_CHECK(encode_camera_shot(bad).empty());
  bad = turned;
  bad.rotation[0] = bad.rotation[1] = bad.rotation[2] = bad.rotation[3] = 0.0f;
  KCD2S_CHECK(encode_camera_shot(bad).empty());

  auto message = encode_camera_shot(sample_shot());
  message[13] = 2;  // the aim byte after the tag and the position
  KCD2S_CHECK(!decode_camera_shot(message, decoded));

  KCD2S_CHECK(decode_camera_clear(encode_camera_clear()));
  KCD2S_CHECK(!decode_camera_clear({kCameraClearTag, 0}));
  CameraShotEnd reason = CameraShotEnd::Lost;
  KCD2S_CHECK(
      decode_camera_shot_ended(encode_camera_shot_ended(CameraShotEnd::WorldChanged), reason));
  KCD2S_CHECK(reason == CameraShotEnd::WorldChanged);
  KCD2S_CHECK(!decode_camera_shot_ended({kCameraShotEndedTag, kCameraShotEndCount}, reason));
}

// The ray reaches only a ready player's client, and its answer comes back to the server under the
// player who traced it.
KCD2S_TEST(world_ray, travels_to_a_ready_client_and_back) {
  Session session;
  KCD2S_CHECK(!session.server->send_world_raycast(session.avatar, sample_ray()));
  KCD2S_CHECK(session.ready());
  KCD2S_CHECK(session.server->send_world_raycast(session.avatar, sample_ray()));
  KCD2S_CHECK(!session.server->send_world_raycast(session.avatar + 1, sample_ray()));
  session.pump(5);
  auto rays = session.client->take_world_rays();
  KCD2S_CHECK(rays.size() == 1 && rays[0] == sample_ray());
  KCD2S_CHECK(session.client->take_world_rays().empty());

  KCD2S_CHECK(session.client->send_world_ray_reply(sample_hit()));
  session.pump(5);
  auto replies = session.server->take_world_ray_replies();
  KCD2S_CHECK(replies.size() == 1);
  KCD2S_CHECK(replies[0].first == session.avatar && replies[0].second == sample_hit());
}

// Shots and clears reach the client in order; a shot the client ended reaches the server.
KCD2S_TEST(camera_shot, travels_to_a_ready_client_in_order) {
  Session session;
  KCD2S_CHECK(!session.server->send_camera_shot(session.avatar, sample_shot()));
  KCD2S_CHECK(session.ready());
  KCD2S_CHECK(session.server->send_camera_shot(session.avatar, sample_shot()));
  KCD2S_CHECK(session.server->send_camera_clear(session.avatar));
  session.pump(5);
  auto orders = session.client->take_camera_orders();
  KCD2S_CHECK(orders.size() == 2);
  KCD2S_CHECK(orders[0].has_value() && *orders[0] == sample_shot());
  KCD2S_CHECK(!orders[1].has_value());

  KCD2S_CHECK(session.client->send_camera_shot_ended(CameraShotEnd::Lost));
  session.pump(5);
  auto ends = session.server->take_camera_shot_ends();
  KCD2S_CHECK(ends.size() == 1 && ends[0].first == session.avatar &&
              ends[0].second == CameraShotEnd::Lost);
}

namespace {

// What a sword met: a wall's brush, a metre and a half from the player's feet.
WorldRayHit sample_weapon_hit(float x) {
  WorldRayHit hit;
  hit.position[0] = x;
  hit.position[2] = 1.2f;
  hit.normal[1] = -1.0f;
  hit.distance = 1.5f;
  hit.surface = "mat_wood";
  hit.kind = RayHitKind::Brush;
  hit.model = "objects/buildings/houses/wall_a.cgf";
  hit.object = RayObject::Brush;
  hit.object_at[0] = x;
  return hit;
}

}  // namespace

// WEAPON_HIT carries one hit as WORLD_RAYCAST_REPLY does, and never a body.
KCD2S_TEST(weapon_hit, round_trips_and_refuses_a_body) {
  const WorldRayHit hit = sample_weapon_hit(3.0f);
  WorldRayHit decoded;
  KCD2S_CHECK(decode_weapon_hit(encode_weapon_hit(hit), decoded));
  KCD2S_CHECK(decoded == hit);
  WorldRayHit body = hit;
  body.entity = 17;
  KCD2S_CHECK(encode_weapon_hit(body).empty());
  WorldRayHit far = hit;
  far.position[0] = kMaxRayCoordinate * 2;
  KCD2S_CHECK(encode_weapon_hit(far).empty());
  auto message = encode_weapon_hit(hit);
  message.push_back(0);
  KCD2S_CHECK(!decode_weapon_hit(message, decoded));
  message = encode_weapon_hit(hit);
  message.resize(message.size() - 4);
  KCD2S_CHECK(!decode_weapon_hit(message, decoded));
  KCD2S_CHECK(!decode_weapon_hit({kWorldRaycastReplyTag}, decoded));
}

// The server takes a ready player's hits within reach of where it has the player, at most
// kMaxWeaponHitsPerSecond a second.
KCD2S_TEST(weapon_hit, server_takes_hits_within_reach_and_rate) {
  Session session;
  KCD2S_CHECK(!session.client->send_weapon_hit(sample_weapon_hit(3.0f)));
  KCD2S_CHECK(session.ready());
  KCD2S_CHECK(session.client->send_weapon_hit(sample_weapon_hit(3.0f)));
  KCD2S_CHECK(session.client->send_weapon_hit(sample_weapon_hit(kWeaponHitReach + 1.0f)));
  session.pump(5);
  auto hits = session.server->take_weapon_hits();
  KCD2S_CHECK(hits.size() == 1);
  KCD2S_CHECK(hits[0].first == session.avatar && hits[0].second == sample_weapon_hit(3.0f));

  session.pump(70);
  for (unsigned i = 0; i < kMaxWeaponHitsPerSecond + 4; ++i) {
    KCD2S_CHECK(session.client->send_weapon_hit(sample_weapon_hit(1.0f)));
  }
  session.pump(5);
  KCD2S_CHECK_EQ(session.server->take_weapon_hits().size(), std::size_t(kMaxWeaponHitsPerSecond));
  session.pump(70);
  KCD2S_CHECK(session.client->send_weapon_hit(sample_weapon_hit(1.0f)));
  session.pump(5);
  KCD2S_CHECK(session.server->take_weapon_hits().size() == 1);
}
