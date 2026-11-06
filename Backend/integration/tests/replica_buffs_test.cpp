#include "../local/movement.hpp"
#include "kcd2s/integration/replica_buffs.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// The owner's fighting buffs on its replicas: the replicaBuffs field.
namespace {

ReplicaBuff buff(std::uint8_t first) {
  ReplicaBuff entry;
  entry.buff[0] = first;
  entry.buff[15] = 0x42;
  return entry;
}

ReplicaBuff injury(std::uint8_t first, std::uint32_t type, std::uint8_t flags,
                   std::int32_t severity) {
  ReplicaBuff entry = buff(first);
  entry.injury = InjuryState{type, flags, severity};
  return entry;
}

}  // namespace

KCD2S_TEST(replica_buffs, round_trip_sorted_with_repeats) {
  const std::vector<ReplicaBuff> buffs = {buff(3), buff(1), buff(3)};
  const std::string field = replica_buffs_field(buffs);
  std::vector<ReplicaBuff> parsed;
  KCD2S_CHECK(parse_replica_buffs_field(field, parsed));
  KCD2S_CHECK_EQ(parsed.size(), std::size_t(3));
  KCD2S_CHECK(parsed[0] == buff(1));
  KCD2S_CHECK(parsed[1] == buff(3));
  KCD2S_CHECK(parsed[2] == buff(3));
  KCD2S_CHECK_EQ(replica_buffs_field(parsed), field);
}

KCD2S_TEST(replica_buffs, an_injury_carries_the_games_numbers) {
  const std::vector<ReplicaBuff> buffs = {injury(2, 1, 1, 22016), buff(1)};
  const std::string field = replica_buffs_field(buffs);
  KCD2S_CHECK(field.find(":1:1:22016") != std::string::npos);
  std::vector<ReplicaBuff> parsed;
  KCD2S_CHECK(parse_replica_buffs_field(field, parsed));
  KCD2S_CHECK_EQ(parsed.size(), std::size_t(2));
  KCD2S_CHECK(parsed[0] == buff(1));
  KCD2S_CHECK(parsed[1] == injury(2, 1, 1, 22016));
  // Out of range numbers do not travel.
  KCD2S_CHECK_EQ(replica_buffs_field({injury(2, 1, 4, 100)}), std::string());
  KCD2S_CHECK_EQ(replica_buffs_field({injury(2, 1, 1, kInjurySeverityOne + 1)}), std::string());
  const std::string guid = guid_text(buff(2).buff);
  for (const std::string& bad : {guid + ":1:1", guid + ":1:4:100", guid + ":1:1:40000",
                                 guid + ":x:1:100", guid + ":1:1:100:"}) {
    KCD2S_CHECK(!parse_replica_buffs_field(bad, parsed));
  }
}

KCD2S_TEST(replica_buffs, the_drunkenness_buff_carries_its_amount) {
  ReplicaBuff drunk = buff(4);
  drunk.amount = 1250;
  const std::vector<ReplicaBuff> buffs = {drunk, buff(1)};
  const std::string field = replica_buffs_field(buffs);
  KCD2S_CHECK(field.find("=1250") != std::string::npos);
  std::vector<ReplicaBuff> parsed;
  KCD2S_CHECK(parse_replica_buffs_field(field, parsed));
  KCD2S_CHECK_EQ(parsed.size(), std::size_t(2));
  KCD2S_CHECK(parsed[1] == drunk);
  ReplicaBuff too_much = buff(4);
  too_much.amount = kDrunkennessMax + 1;
  KCD2S_CHECK_EQ(replica_buffs_field({too_much}), std::string());
  const std::string guid = guid_text(buff(4).buff);
  for (const std::string& bad : {guid + "=", guid + "=x", guid + "=12345678"}) {
    KCD2S_CHECK(!parse_replica_buffs_field(bad, parsed));
  }
}

KCD2S_TEST(replica_buffs, empty_is_none) {
  KCD2S_CHECK_EQ(replica_buffs_field({}), std::string());
  std::vector<ReplicaBuff> parsed = {buff(1)};
  KCD2S_CHECK(parse_replica_buffs_field("", parsed));
  KCD2S_CHECK(parsed.empty());
}

KCD2S_TEST(replica_buffs, refuses_what_the_owner_does_not_write) {
  std::vector<ReplicaBuff> many(kMaxReplicaBuffs + 1, buff(1));
  KCD2S_CHECK_EQ(replica_buffs_field(many), std::string());
  const std::string one = guid_text(buff(1).buff);
  const std::string two = guid_text(buff(2).buff);
  std::vector<ReplicaBuff> parsed = {buff(9)};
  for (const std::string& bad : {two + "," + one, one + ",", "," + one, one + ";" + two,
                                 one.substr(1), one + "," + two.substr(0, 35)}) {
    KCD2S_CHECK(!parse_replica_buffs_field(bad, parsed));
  }
  KCD2S_CHECK_EQ(parsed.size(), std::size_t(1));
  std::string too_many;
  for (std::size_t i = 0; i <= kMaxReplicaBuffs; ++i) {
    too_many += (i > 0 ? "," : "") + one;
  }
  KCD2S_CHECK(!parse_replica_buffs_field(too_many, parsed));
}

KCD2S_TEST(replica_buffs, mirrors_fighting_classes_only) {
  KCD2S_CHECK(replica_buff_class(13));   // potion
  KCD2S_CHECK(replica_buff_class(10));   // alcohol
  KCD2S_CHECK(replica_buff_class(5));    // injury, with the game's numbers of it
  KCD2S_CHECK(!replica_buff_class(4));   // perk: the replica's perks make these
  KCD2S_CHECK(!replica_buff_class(16));  // unconsciousness: follows the owner's knock-out
  KCD2S_CHECK(!replica_buff_class(7));   // poison: health follows the owner
}

KCD2S_TEST(replica_buffs, field_is_the_owners) {
  const auto schema = local::player_schema();
  const auto* field = schema.get("NetworkPlayer")->find("replicaBuffs");
  KCD2S_CHECK(field != nullptr);
  KCD2S_CHECK(field->id == local::ReplicaBuffs);
  KCD2S_CHECK(field->kind == replication::FieldKind::Str);
  KCD2S_CHECK(field->auth == replication::Authority::Owner);
}
