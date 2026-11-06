#include "../local/movement.hpp"
#include "kcd2s/integration/replica_soul.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// The owner's RPG soul on its replicas: the replicaSoul field.
namespace {

// "Pacifist" in rpg/perk__kcd2.xml: 2b9f3c56-d5f4-49b0-a259-8a89a67c8f0d, as the game stores it.
constexpr PerkId kPacifist = {0x56, 0x3c, 0x9f, 0x2b, 0xf4, 0xd5, 0xb0, 0x49,
                              0xa2, 0x59, 0x8a, 0x89, 0xa6, 0x7c, 0x8f, 0x0d};

ReplicaSoul sample() {
  ReplicaSoul soul;
  soul.levels.fill(5);
  soul.levels[8] = 30;  // weapon_sword
  soul.levels[15] = 0;  // strength
  PerkId other{};
  other[0] = 1;
  soul.perks = {kPacifist, other};
  normalize_perks(soul.perks);
  return soul;
}

}  // namespace

KCD2S_TEST(replica_soul, round_trip) {
  const ReplicaSoul soul = sample();
  const std::string field = replica_soul_field(soul);
  KCD2S_CHECK(field ==
              "5,5,5,5,5,5,5,5,30,5,5,5,5,5,5,0,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5;"
              "00000001-0000-0000-0000-000000000000,2b9f3c56-d5f4-49b0-a259-8a89a67c8f0d");
  ReplicaSoul parsed;
  KCD2S_CHECK(parse_replica_soul_field(field, parsed));
  KCD2S_CHECK(parsed == soul);
}

KCD2S_TEST(replica_soul, no_perks) {
  ReplicaSoul soul;
  const std::string field = replica_soul_field(soul);
  KCD2S_CHECK(field == "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0;");
  ReplicaSoul parsed = sample();
  KCD2S_CHECK(parse_replica_soul_field(field, parsed));
  KCD2S_CHECK(parsed == soul);
}

KCD2S_TEST(replica_soul, what_cannot_travel_is_empty) {
  ReplicaSoul soul = sample();
  soul.levels[0] = kLevelCap + 1;
  KCD2S_CHECK(replica_soul_field(soul).empty());
  soul = sample();
  soul.perks.resize(kMaxPerks + 1);
  KCD2S_CHECK(replica_soul_field(soul).empty());
}

KCD2S_TEST(replica_soul, malformed_fields_are_refused) {
  const std::string good = replica_soul_field(sample());
  ReplicaSoul parsed = sample();
  const std::string levels = "5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5";
  const std::string perk = "2b9f3c56-d5f4-49b0-a259-8a89a67c8f0d";
  for (const std::string& bad : std::vector<std::string>{
           std::string(),                                                  // not reported yet
           levels,                                                         // no ';'
           std::string("5,5,5;"),                                          // too few levels
           levels + ",5;",                                                 // too many levels
           "31" + levels.substr(1) + ";",                                  // past the cap
           "-1" + levels.substr(1) + ";",                                  // not a level
           "005" + levels.substr(1) + ";",                                 // too many digits
           levels + ";" + perk + ",",                                      // trailing comma
           levels + ";" + perk.substr(1),                                  // short GUID
           levels + ";" + perk + "," + perk,                               // duplicate
           levels + ";" + perk + ",00000001-0000-0000-0000-000000000000",  // out of order
       }) {
    KCD2S_CHECK(!parse_replica_soul_field(bad, parsed));
  }
  KCD2S_CHECK(parsed == sample());
  KCD2S_CHECK(parse_replica_soul_field(good, parsed));
}

KCD2S_TEST(replica_soul, carries_every_stat_and_skill_of_the_games_tables) {
  // rpg/stat.xml and rpg/skill.xml of WHGame 1.5.5.
  const std::array<unsigned, 5> stats = {0, 1, 2, 3, 9};
  const std::array<unsigned, 30> skills = {0,  1,  2,  3,  4,  6,  7,  8,  10, 11,
                                           12, 13, 14, 15, 16, 17, 19, 20, 22, 23,
                                           24, 26, 27, 28, 29, 30, 31, 32, 33, 34};
  const auto carried = [](bool stat, unsigned id) {
    return std::count_if(kReplicaTracks.begin(), kReplicaTracks.end(), [&](const TrackId& track) {
             return track.stat == stat && track.id == id;
           }) == 1;
  };
  for (unsigned id : stats) {
    KCD2S_CHECK(carried(true, id));
  }
  for (unsigned id : skills) {
    KCD2S_CHECK(carried(false, id));
  }
  KCD2S_CHECK_EQ(kReplicaTracks.size(), stats.size() + skills.size());
}

KCD2S_TEST(replica_soul, field_is_the_owners) {
  const auto schema = local::player_schema();
  const auto* field = schema.get("NetworkPlayer")->find("replicaSoul");
  KCD2S_CHECK(field != nullptr);
  KCD2S_CHECK(field->id == local::ReplicaSoul);
  KCD2S_CHECK(field->kind == replication::FieldKind::Str);
  KCD2S_CHECK(field->auth == replication::Authority::Owner);
}
