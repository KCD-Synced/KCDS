#include "kcd2s/integration/doors.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

namespace {

DoorEntry sample_entry() {
  DoorEntry door;
  door.state.guid = 0x0123456789ABCDEFull;
  door.state.open = true;
  door.state.side = true;
  door.state.locked = false;
  door.name = "door_inn_front[Trosky/inn:prefab_0123456789ABCDEF]";
  door.position[0] = 1200.5f;
  door.position[1] = -330.25f;
  door.position[2] = 88.0f;
  door.canBePicked = true;
  door.key = "0123456789abcdef0123456789abcdef";
  door.builtLocked = true;
  door.difficulty = 0.65f;
  door.model = "objects/characters/assets/doors/lvl1_door_a_left.cdf";
  return door;
}

DoorSpawn sample_spawn() {
  DoorSpawn door;
  door.guid = 0x6b63647300000001ull;
  door.model = "objects/characters/assets/doors/lvl1_door_a_left.cdf";
  door.position[0] = 1650.5f;
  door.position[1] = 2160.0f;
  door.position[2] = 35.4f;
  door.rotation[2] = 0.7071068f;
  door.rotation[3] = 0.7071068f;
  door.anim_set = "VillageLeft";
  door.helpers = "DoorLeft";
  door.smart_object_type = "49515779-1a9d-330c-4768-c9326ea1ca96";
  door.rigid = true;
  door.pushable = true;
  door.locked = true;
  door.lock_types = "manual/key";
  door.canBePicked = false;
  door.difficulty = 0.8f;
  door.key = "d14a857c-e2fb-4a8a-a54b-79dde42d06cd";
  return door;
}

}  // namespace

KCD2S_TEST(doors, guid_text_round_trips) {
  KCD2S_CHECK(door_guid_text(0x0123456789ABCDEFull) == "0123456789abcdef");
  std::uint64_t guid = 0;
  KCD2S_CHECK(parse_door_guid("0123456789ABCDEF", guid));
  KCD2S_CHECK(guid == 0x0123456789ABCDEFull);
  KCD2S_CHECK(!parse_door_guid("0123456789abcde", guid));
  KCD2S_CHECK(!parse_door_guid("0123456789abcdeg", guid));
}

KCD2S_TEST(doors, door_set_round_trips) {
  DoorState state;
  state.guid = 77;
  state.open = true;
  state.side = true;
  state.locked = true;
  DoorState decoded;
  KCD2S_CHECK(decode_door_set(encode_door_set(state), decoded));
  KCD2S_CHECK(decoded == state);
  KCD2S_CHECK(decoded.difficulty == -1.0f);  // the level's own
  // A script's lock-picking difficulty.
  state.difficulty = 0.9f;
  KCD2S_CHECK(decode_door_set(encode_door_set(state), decoded));
  KCD2S_CHECK(decoded.difficulty == 0.9f);
  state.difficulty = 1.5f;
  KCD2S_CHECK(encode_door_set(state).empty());
  state.difficulty = -1.0f;
  state.guid = 0;
  KCD2S_CHECK(encode_door_set(state).empty());
  KCD2S_CHECK(!decode_door_set({kDoorSetTag, 1, 2}, decoded));
}

KCD2S_TEST(doors, nearest_round_trips) {
  std::uint32_t request = 0;
  KCD2S_CHECK(decode_door_nearest(encode_door_nearest(4242), request));
  KCD2S_CHECK(request == 4242);
  KCD2S_CHECK(!decode_door_nearest({kDoorNearestTag, 1}, request));
}

KCD2S_TEST(doors, report_round_trips) {
  std::vector<DoorEntry> doors = {sample_entry(), sample_entry()};
  doors[1].state.guid = 5;
  doors[1].key.clear();
  std::vector<DoorEntry> decoded;
  KCD2S_CHECK(decode_door_report(encode_door_report(doors), decoded));
  KCD2S_CHECK(decoded == doors);
}

KCD2S_TEST(doors, report_refuses_bad_input) {
  KCD2S_CHECK(encode_door_report({}).empty());
  std::vector<DoorEntry> many(kMaxDoorsPerReport + 1, sample_entry());
  KCD2S_CHECK(encode_door_report(many).empty());
  std::vector<DoorEntry> one = {sample_entry()};
  one[0].name.assign(kMaxDoorNameLength + 1, 'x');
  KCD2S_CHECK(encode_door_report(one).empty());
  one = {sample_entry()};
  one[0].position[1] = kMaxDoorCoordinate * 2.0f;
  KCD2S_CHECK(encode_door_report(one).empty());
  one = {sample_entry()};
  one[0].state.guid = 0;
  KCD2S_CHECK(encode_door_report(one).empty());
  std::vector<std::uint8_t> bytes = encode_door_report({sample_entry()});
  bytes.resize(bytes.size() - 4);
  std::vector<DoorEntry> decoded;
  KCD2S_CHECK(!decode_door_report(bytes, decoded));
}

KCD2S_TEST(doors, use_round_trips) {
  for (std::uint8_t action = 0; action < kDoorActionCount; ++action) {
    DoorUse use;
    use.guid = 99;
    use.action = DoorAction(action);
    use.keySide = action % 2 == 1;
    DoorUse decoded;
    KCD2S_CHECK(decode_door_use(encode_door_use(use), decoded));
    KCD2S_CHECK(decoded == use);
  }
  // An NPC's use (issue 016) carries the NPC.
  DoorUse by_npc;
  by_npc.guid = 99;
  by_npc.action = DoorAction::Open;
  by_npc.npc = 6738;
  DoorUse read_back;
  KCD2S_CHECK(decode_door_use(encode_door_use(by_npc), read_back));
  KCD2S_CHECK(read_back == by_npc && read_back.npc == 6738);
  DoorUse bad;
  bad.guid = 99;
  bad.action = DoorAction(kDoorActionCount);
  KCD2S_CHECK(encode_door_use(bad).empty());
}

KCD2S_TEST(doors, nearest_reply_round_trips) {
  DoorNearestReply found;
  found.request = 9;
  found.found = true;
  found.door = sample_entry();
  DoorNearestReply decoded;
  KCD2S_CHECK(decode_door_nearest_reply(encode_door_nearest_reply(found), decoded));
  KCD2S_CHECK(decoded == found);

  DoorNearestReply missing;
  missing.request = 10;
  missing.reason = "no door in sight";
  KCD2S_CHECK(decode_door_nearest_reply(encode_door_nearest_reply(missing), decoded));
  KCD2S_CHECK(decoded == missing);
}

KCD2S_TEST(doors, spawn_round_trips) {
  static_assert(kDoorSpawnTag == 157);
  DoorSpawn decoded;
  const DoorSpawn hung = sample_spawn();
  KCD2S_CHECK(decode_door_spawn(encode_door_spawn(hung), decoded));
  KCD2S_CHECK(decoded == hung);
  // The game's own difficulty travels as -1.
  DoorSpawn own = sample_spawn();
  own.difficulty = -0.5f;
  KCD2S_CHECK(decode_door_spawn(encode_door_spawn(own), decoded));
  KCD2S_CHECK(decoded.difficulty == -1.0f);
  // Taken down: only the GUID.
  DoorSpawn down;
  down.guid = hung.guid;
  down.remove = true;
  KCD2S_CHECK(decode_door_spawn(encode_door_spawn(down), decoded));
  KCD2S_CHECK(decoded == down);
  KCD2S_CHECK(encode_door_spawn(down).size() < 12);
}

KCD2S_TEST(doors, spawn_refuses_bad_input) {
  DoorSpawn door = sample_spawn();
  door.lock_types = "key/none";
  KCD2S_CHECK(encode_door_spawn(door).empty());
  door = sample_spawn();
  door.model.clear();
  KCD2S_CHECK(encode_door_spawn(door).empty());
  door = sample_spawn();
  door.position[1] = std::nanf("");
  KCD2S_CHECK(encode_door_spawn(door).empty());
  door = sample_spawn();
  door.rotation[0] = 2.0f;
  KCD2S_CHECK(encode_door_spawn(door).empty());
  door = sample_spawn();
  door.difficulty = 1.5f;
  KCD2S_CHECK(encode_door_spawn(door).empty());
  door = sample_spawn();
  door.guid = 0;
  KCD2S_CHECK(encode_door_spawn(door).empty());
  auto cut = encode_door_spawn(sample_spawn());
  cut.resize(cut.size() - 3);
  DoorSpawn decoded;
  KCD2S_CHECK(!decode_door_spawn(cut, decoded));
  KCD2S_CHECK(!decode_door_spawn({kDoorSetTag, 1}, decoded));
}
