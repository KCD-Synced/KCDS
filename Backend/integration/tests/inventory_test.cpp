#include <limits>

#include "kcd2s/integration/damage.hpp"
#include "kcd2s/integration/inventory.hpp"
#include "kcd2s/integration/item_icon.hpp"
#include "kcd2s/integration/item_overrides.hpp"
#include "kcd2s/integration/item_types.hpp"
#include "kcd2s/integration/shots.hpp"
#include "kcd2s/integration/world_items.hpp"
#include "kcd2s/integration/stash.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// A player's inventory: INVENTORY_SHOW and INVENTORY_REPORT.
namespace {

InventoryRowState sword(const char* id, std::uint32_t amount) {
  InventoryRowState row;
  row.id = id;
  parse_guid("00cca9e3-8ef2-46db-8cbf-86ec51930919", row.item);  // longSwordDuel
  row.amount = amount;
  row.quality = 4;
  row.health = 0.725f;
  row.equipped = 1;
  return row;
}

}  // namespace

KCD2S_TEST(inventory, show_round_trip) {
  InventoryShow show;
  show.revision = 12345;
  show.replace = true;
  show.rows = {sword("i1", 2), sword("blade", 1)};
  // A dirty, bloody row keeps its grime on the way.
  show.rows[1].grime = {128, 10, 0, 0, 255, 0, 3};
  InventoryShow decoded;
  KCD2S_CHECK(decode_inventory_show(encode_inventory_show(show), decoded) && decoded == show);
  show.rows.clear();
  KCD2S_CHECK(decode_inventory_show(encode_inventory_show(show), decoded) && decoded.rows.empty());
}

KCD2S_TEST(inventory, show_refuses_what_cannot_travel) {
  InventoryShow show;
  show.rows = {sword("", 1)};
  KCD2S_CHECK(encode_inventory_show(show).empty());
  show.rows = {sword(std::string(kMaxRowIdBytes + 1, 'x').c_str(), 1)};
  KCD2S_CHECK(encode_inventory_show(show).empty());
  show.rows = std::vector<InventoryRowState>(kMaxInventoryRows + 1, sword("i1", 1));
  KCD2S_CHECK(encode_inventory_show(show).empty());
  // More equipped units than the row holds, a broken health and a cut message do not decode.
  InventoryShow decoded;
  show.rows = {sword("i1", 1)};
  show.rows[0].equipped = 2;
  KCD2S_CHECK(!decode_inventory_show(encode_inventory_show(show), decoded));
  show.rows = {sword("i1", 1)};
  show.rows[0].health = 1.5f;
  KCD2S_CHECK(!decode_inventory_show(encode_inventory_show(show), decoded));
  show.rows = {sword("i1", 1)};
  auto message = encode_inventory_show(show);
  message.pop_back();
  KCD2S_CHECK(!decode_inventory_show(message, decoded));
}

KCD2S_TEST(inventory, report_round_trip) {
  InventoryReport shown;
  shown.kind = InventoryReport::Kind::Shown;
  shown.revision = 7;
  InventoryReport decoded;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(shown), decoded) && decoded == shown);
  InventoryReport census;
  census.kind = InventoryReport::Kind::Census;
  census.rows = {sword("", 1), sword("", 3)};
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(census), decoded) &&
              decoded == census);
  census.rows = std::vector<InventoryRowState>(kMaxCensusRows + 1, sword("", 1));
  KCD2S_CHECK(encode_inventory_report(census).empty());
  KCD2S_CHECK(!decode_inventory_report({kInventoryReportTag, 9}, decoded));
}

KCD2S_TEST(inventory, native_changes_round_trip) {
  InventoryReport report;
  report.kind = InventoryReport::Kind::Changes;
  report.revision = 4;
  report.epoch = 3;
  report.sequence = 17;
  InventoryNativeChange lost;
  lost.kind = InventoryNativeChange::Kind::Lost;
  lost.id = "i2";
  lost.amount = 1;
  InventoryNativeChange worn = lost;
  worn.kind = InventoryNativeChange::Kind::Worn;
  worn.quality = 3;
  worn.health = 0.41f;
  InventoryNativeChange gained;
  gained.kind = InventoryNativeChange::Kind::Gained;
  parse_guid("00cca9e3-8ef2-46db-8cbf-86ec51930919", gained.item);
  gained.amount = 5;
  // A shirt that got dirty and bloody in two zones.
  InventoryNativeChange soiled = lost;
  soiled.kind = InventoryNativeChange::Kind::Soiled;
  soiled.grime = {200, 0, 90, 0, 0, 255, 0};
  report.changes = {lost, worn, gained, soiled};
  InventoryReport decoded;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(report), decoded) &&
              decoded == report);
  // Nothing to report, a row change without its row and a Gained without units do not travel.
  report.changes.clear();
  KCD2S_CHECK(encode_inventory_report(report).empty());
  lost.id.clear();
  report.changes = {lost};
  KCD2S_CHECK(encode_inventory_report(report).empty());
  gained.amount = 0;
  report.changes = {gained};
  KCD2S_CHECK(!encode_inventory_report(report).empty());
  KCD2S_CHECK(!decode_inventory_report(encode_inventory_report(report), decoded));
  // A batch has a number from 1.
  gained.amount = 5;
  report.changes = {gained};
  report.sequence = 0;
  KCD2S_CHECK(encode_inventory_report(report).empty());
}

KCD2S_TEST(inventory, show_carries_epoch) {
  InventoryShow show;
  show.revision = 9;
  show.epoch = 0xFFFFFFFFu;
  show.rows = {sword("i1", 2)};
  InventoryShow decoded;
  KCD2S_CHECK(decode_inventory_show(encode_inventory_show(show), decoded) && decoded == show);
}

KCD2S_TEST(inventory, changes_ack_round_trip) {
  InventoryChangesAck ack;
  ack.epoch = 3;
  ack.sequence = 17;
  ack.verdict = InventoryChangesAck::Verdict::Busy;
  ack.revision = 1234567;
  InventoryChangesAck decoded;
  KCD2S_CHECK(decode_inventory_changes_ack(encode_inventory_changes_ack(ack), decoded) &&
              decoded == ack);
  ack.verdict = InventoryChangesAck::Verdict::Refused;
  KCD2S_CHECK(decode_inventory_changes_ack(encode_inventory_changes_ack(ack), decoded) &&
              decoded == ack);
  // An unknown verdict, a cut message and another tag do not decode.
  auto message = encode_inventory_changes_ack(ack);
  message[3] = 9;
  KCD2S_CHECK(!decode_inventory_changes_ack(message, decoded));
  message = encode_inventory_changes_ack(ack);
  message.pop_back();
  KCD2S_CHECK(!decode_inventory_changes_ack(message, decoded));
  KCD2S_CHECK(!decode_inventory_changes_ack({kInventoryConsumeTag, 1, 1, 0, 0}, decoded));
}

KCD2S_TEST(inventory, item_names_round_trip) {
  ItemNames names;
  names.full = true;
  names.names = {{"ui_nm_longSwordDuel", "Oathkeeper"}, {"ui_nm_apple", ""}};
  ItemNames decoded;
  KCD2S_CHECK(decode_item_names(encode_item_names(names), decoded) && decoded == names);
  names.names = {{"", "No key"}};
  KCD2S_CHECK(encode_item_names(names).empty());
  names.names = {{"ui_nm_apple", std::string(kMaxItemTextBytes + 1, 'x')}};
  KCD2S_CHECK(encode_item_names(names).empty());
  names.names = {{std::string(kMaxItemNameBytes + 1, 'k'), "A name"}};
  KCD2S_CHECK(encode_item_names(names).empty());
}

// The current outfit's quickslots: on a restored stock, and as a report.
KCD2S_TEST(inventory, quickslots_round_trip) {
  InventoryQuickslots slots;
  slots.primary[0] = "i1";
  slots.secondary[0] = "shield";
  slots.primary[2] = "blade";
  slots.items[3] = "i7";
  slots.active_weapon = 2;
  slots.active_item = 3;
  InventoryShow show;
  show.revision = 3;
  show.rows = {sword("i1", 1)};
  show.quickslots = slots;
  InventoryShow shown;
  KCD2S_CHECK(decode_inventory_show(encode_inventory_show(show), shown) && shown == show);
  InventoryReport report;
  report.kind = InventoryReport::Kind::Quickslots;
  report.quickslots = slots;
  InventoryReport decoded;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(report), decoded) &&
              decoded == report);
  // A selected slot past the fourth or an id too long does not travel.
  report.quickslots.active_item = 4;
  KCD2S_CHECK(encode_inventory_report(report).empty());
  report.quickslots.active_item = 0;
  report.quickslots.items[0] = std::string(kMaxRowIdBytes + 1, 'x');
  KCD2S_CHECK(encode_inventory_report(report).empty());
  show.quickslots->active_weapon = 9;
  KCD2S_CHECK(encode_inventory_show(show).empty());
}

// A use the server decides: the request, the verdict and the receipt.
KCD2S_TEST(inventory, consumption_round_trip) {
  InventoryReport consume;
  consume.kind = InventoryReport::Kind::Consume;
  consume.request = 70000;
  consume.row = "apples";
  consume.revision = 12;
  InventoryReport decoded;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(consume), decoded) &&
              decoded == consume);
  consume.row.clear();
  KCD2S_CHECK(encode_inventory_report(consume).empty());
  InventoryReport consumed;
  consumed.kind = InventoryReport::Kind::Consumed;
  consumed.request = 70000;
  consumed.used = true;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(consumed), decoded) &&
              decoded == consumed);
  InventoryConsume verdict;
  verdict.request = 70000;
  verdict.verdict = InventoryConsume::Verdict::Plain;
  InventoryConsume heard;
  KCD2S_CHECK(decode_inventory_consume(encode_inventory_consume(verdict), heard) &&
              heard == verdict);
  // A stack taken up and units dropped.
  InventoryReport pickup;
  pickup.kind = InventoryReport::Kind::Pickup;
  pickup.request = 42;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(pickup), decoded) &&
              decoded == pickup);
  InventoryReport drop;
  drop.kind = InventoryReport::Kind::Drop;
  drop.row = "blade";
  drop.request = 3;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(drop), decoded) && decoded == drop);
  drop.request = 0;
  KCD2S_CHECK(encode_inventory_report(drop).empty());
  InventoryReport pot;
  pot.kind = InventoryReport::Kind::PotConsume;
  pot.request = 9;
  parse_guid("2264f217-590e-4c0f-a4c6-f50c6532b9f6", pot.item);
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(pot), decoded) && decoded == pot);
  // With the fireplace the player's game ate from.
  parse_guid("29410bf0-15c5-4b8a-9d2e-7c3f1a6b5d40", pot.pot);
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(pot), decoded) && decoded == pot &&
              guid_text(decoded.pot) == "29410bf0-15c5-4b8a-9d2e-7c3f1a6b5d40");
  // Health a player's game took off them.
  PlayerDamage damage;
  damage.amount = 10.5f;
  damage.attacker = 4097;
  damage.reason = 1;
  damage.subpart = 24;
  PlayerDamage damage_decoded;
  KCD2S_CHECK(decode_player_damage(encode_player_damage(damage), damage_decoded) &&
              damage_decoded == damage);
  damage.subpart = -1;
  KCD2S_CHECK(decode_player_damage(encode_player_damage(damage), damage_decoded) &&
              damage_decoded == damage);
  damage.reason = 11;
  KCD2S_CHECK(encode_player_damage(damage).empty());
  damage.reason = 7;
  damage.amount = 0;
  KCD2S_CHECK(encode_player_damage(damage).empty());
  std::uint32_t opponent = 0;
  KCD2S_CHECK(decode_combat_cancel(encode_combat_cancel(4097), opponent) && opponent == 4097);
  // COMBAT_START: the same shape, its own tag.
  KCD2S_CHECK(decode_combat_start(encode_combat_start(4099), opponent) && opponent == 4099);
  KCD2S_CHECK(encode_combat_start(0).empty());
  KCD2S_CHECK(!decode_combat_start(encode_combat_cancel(4099), opponent));
  KCD2S_CHECK(!decode_combat_cancel(encode_combat_start(4099), opponent));
  // A blow held for the server's ruling, and the ruling.
  PlayerHitReport hit{7, 4098, 13.66f, 40.0f, true, {}};
  hit.weapon[0] = 0xE3;
  PlayerHitReport hit_back;
  KCD2S_CHECK(decode_player_hit(encode_player_hit(hit), hit_back) && hit_back == hit);
  hit.armed = false;
  KCD2S_CHECK(decode_player_hit(encode_player_hit(hit), hit_back) && hit_back == hit);
  // An arrow, armed or not.
  hit.missile = true;
  KCD2S_CHECK(decode_player_hit(encode_player_hit(hit), hit_back) && hit_back == hit &&
              hit_back.missile);
  hit.armed = true;
  KCD2S_CHECK(decode_player_hit(encode_player_hit(hit), hit_back) && hit_back == hit);
  hit.armed = false;
  hit.missile = false;
  hit.attacker = 0;
  KCD2S_CHECK(encode_player_hit(hit).empty());
  hit.attacker = 4098;
  hit.damage = -1.0f;
  KCD2S_CHECK(encode_player_hit(hit).empty());
  hit.damage = 13.66f;
  // A melee blow the game priced, with the numbers it priced it with.
  hit.priced = true;
  hit.armed = true;
  hit.price.attack = {0, 42.5f, 3.25f};
  hit.price.armor = {12, 18.5f, 9};
  hit.price.block_defense = 21;
  hit.price.body_coefficient = 1.5f;
  hit.price.subpart = 24;
  hit.price.blocked = true;
  hit.price.block_broken = true;
  KCD2S_CHECK(decode_player_hit(encode_player_hit(hit), hit_back) && hit_back == hit &&
              hit_back.price.subpart == 24 && hit_back.price.blocked &&
              !hit_back.price.perfect_block && hit_back.price.block_broken);
  hit.price.subpart = -1;
  hit.price.blocked = false;
  hit.price.block_broken = false;
  hit.price.perfect_block = true;
  KCD2S_CHECK(decode_player_hit(encode_player_hit(hit), hit_back) && hit_back == hit);
  hit.price.attack[1] = -1.0f;
  KCD2S_CHECK(encode_player_hit(hit).empty());
  hit.price.attack[1] = 42.5f;
  // An arrow is never priced.
  hit.missile = true;
  KCD2S_CHECK(encode_player_hit(hit).empty());
  hit.missile = false;
  auto priced_bytes = encode_player_hit(hit);
  priced_bytes.back() = 8;  // an unknown block flag
  KCD2S_CHECK(!decode_player_hit(priced_bytes, hit_back));
  priced_bytes.pop_back();
  KCD2S_CHECK(!decode_player_hit(priced_bytes, hit_back));
  HitVerdict ruling_back;
  for (const HitVerdict ruling :
       {HitVerdict{7, HitRuling::Set, 20.5f}, HitVerdict{8, HitRuling::Refused, 0},
        HitVerdict{9, HitRuling::Native, 0}, HitVerdict{10, HitRuling::Scaled, 0.5f},
        HitVerdict{11, HitRuling::Priced, 0, {2.0f, 0.5f, 1.0f, 1.25f}}}) {
    KCD2S_CHECK(decode_hit_verdict(encode_hit_verdict(ruling), ruling_back) &&
                ruling_back == ruling);
  }
  KCD2S_CHECK(ruling_back.modifiers.attack == 2.0f && ruling_back.modifiers.health == 1.25f);
  KCD2S_CHECK(encode_hit_verdict(HitVerdict{7, HitRuling::Priced, 0, {-1, 1, 1, 1}}).empty());
  KCD2S_CHECK(encode_hit_verdict(HitVerdict{7, HitRuling(9), 0}).empty());
  KCD2S_CHECK(!decode_hit_verdict({kHitVerdictTag, 1, 2}, ruling_back));
  // A Priced ruling without its modifiers does not decode.
  auto short_priced = encode_hit_verdict(HitVerdict{7, HitRuling::Priced, 0, {}});
  short_priced.resize(short_priced.size() - 4);
  KCD2S_CHECK(!decode_hit_verdict(short_priced, ruling_back));
  // HIT_RULES: whether blows wait for a ruling.
  bool rules = false;
  bool harm = false;
  KCD2S_CHECK(decode_hit_rules(encode_hit_rules(true), rules, harm) && rules && !harm);
  KCD2S_CHECK(decode_hit_rules(encode_hit_rules(false), rules, harm) && !rules && !harm);
  KCD2S_CHECK(decode_hit_rules(encode_hit_rules(false, true), rules, harm) && !rules && harm);
  KCD2S_CHECK(decode_hit_rules(encode_hit_rules(true, true), rules, harm) && rules && harm);
  KCD2S_CHECK(!decode_hit_rules({kHitRulesTag, 4}, rules, harm));
  KCD2S_CHECK(!decode_hit_rules({kHitRulesTag}, rules, harm));
  // PLAYER_HARM: harm the player's game holds for a ruling.
  PlayerHarm fall{9, 23.5f, 7, 55, 0};
  PlayerHarm fall_back;
  KCD2S_CHECK(decode_player_harm(encode_player_harm(fall), fall_back) && fall_back == fall);
  PlayerHarm blow{10, 12.0f, 1, -1, 4099};
  KCD2S_CHECK(decode_player_harm(encode_player_harm(blow), fall_back) && fall_back == blow);
  blow.request = 0;
  KCD2S_CHECK(encode_player_harm(blow).empty());
  blow.request = 10;
  blow.reason = kDamageReasons;
  KCD2S_CHECK(encode_player_harm(blow).empty());
  blow.reason = 1;
  blow.amount = 0;
  KCD2S_CHECK(encode_player_harm(blow).empty());
  auto cut = encode_player_harm(fall);
  cut.pop_back();
  KCD2S_CHECK(!decode_player_harm(cut, fall_back));
  KCD2S_CHECK(encode_combat_cancel(0).empty());
  // Item types a resource registered, and a description as long as one may be.
  ItemTypes types;
  types.full = true;
  ItemType timber;
  timber.id[0] = 1;
  timber.visual[0] = 2;
  timber.name = "village:timber";
  timber.name_key = "kcds_nm_item_1";
  timber.description_key = "kcds_in_item_1";
  timber.use_label_key = "kcds_use_item_1";
  timber.weight = 2.5f;
  timber.price = 12;
  timber.stackable = false;
  timber.usable = true;
  types.types.push_back(timber);
  ItemTypes decoded_types;
  KCD2S_CHECK(decode_item_types(encode_item_types(types), decoded_types) && decoded_types == types);
  // A type with its own icon, shipped by its resource.
  ItemTypes iconic = types;
  iconic.types[0].icon = "kcds_village_icons_letter";
  iconic.types[0].icon_file = "village/icons/letter.dds";
  KCD2S_CHECK(decode_item_types(encode_item_types(iconic), decoded_types) &&
              decoded_types == iconic);
  // A type with a mesh of its own.
  ItemTypes modelled = types;
  modelled.types[0].model = "objects/manmade/common_furniture/bucket/bucket_a_a.cgf";
  KCD2S_CHECK(decode_item_types(encode_item_types(modelled), decoded_types) &&
              decoded_types == modelled);
  iconic.types[0].icon_file.clear();
  KCD2S_CHECK(encode_item_types(iconic).empty());
  types.types[0].name.clear();
  KCD2S_CHECK(encode_item_types(types).empty());
  ItemNames long_names;
  long_names.names.emplace_back("kcds_in_item_1", std::string(kMaxItemTextBytes, 'x'));
  ItemNames long_decoded;
  KCD2S_CHECK(decode_item_names(encode_item_names(long_names), long_decoded) &&
              long_decoded == long_names);
  // A use of a usable registered type.
  InventoryReport custom;
  custom.kind = InventoryReport::Kind::CustomUse;
  custom.row = "i4";
  custom.revision = 9;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(custom), decoded) &&
              decoded == custom);
  // Containers: open, close, take and put.
  InventoryReport stash;
  stash.kind = InventoryReport::Kind::StashOpen;
  stash.request = 77;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(stash), decoded) && decoded == stash);
  stash.kind = InventoryReport::Kind::StashTake;
  stash.row = "r3";
  stash.units = 2;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(stash), decoded) && decoded == stash);
  stash.kind = InventoryReport::Kind::StashPut;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(stash), decoded) && decoded == stash);
  stash.units = 0;
  KCD2S_CHECK(encode_inventory_report(stash).empty());
  // A horse's owner changes its tack in the game's horse inventory screen.
  InventoryReport gear;
  gear.kind = InventoryReport::Kind::HorseGearEquip;
  gear.request = 4118;
  gear.row = "s";
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(gear), decoded) && decoded == gear);
  gear.used = true;  // a row of the horse's saddlebags
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(gear), decoded) && decoded == gear);
  gear.used = false;
  gear.row.clear();
  KCD2S_CHECK(encode_inventory_report(gear).empty());
  gear.kind = InventoryReport::Kind::HorseGearRemove;
  gear.item[0] = 0x66;
  gear.item[15] = 0x2e;
  KCD2S_CHECK(decode_inventory_report(encode_inventory_report(gear), decoded) && decoded == gear);
  gear.item = Guid{};
  KCD2S_CHECK(encode_inventory_report(gear).empty());
  gear.request = 0;
  gear.item[0] = 1;
  KCD2S_CHECK(encode_inventory_report(gear).empty());
  StashShow show;
  show.stash = 77;
  show.open = true;
  show.revision = 4;
  InventoryRowState kept;
  kept.id = "r1";
  kept.item[0] = 9;
  kept.amount = 3;
  kept.quality = 2;
  kept.health = 0.5f;
  show.rows.push_back(kept);
  StashShow shown;
  KCD2S_CHECK(decode_stash_show(encode_stash_show(show), shown) && shown == show);
  // A horse's saddlebags name the horse.
  show.open = false;
  show.horse = 4136;
  KCD2S_CHECK(decode_stash_show(encode_stash_show(show), shown) && shown == show);
  show.horse = 0;
  show.open = true;
  show.closed = true;
  KCD2S_CHECK(encode_stash_show(show).empty());
  auto bytes = encode_inventory_consume(verdict);
  bytes.back() = 3;
  KCD2S_CHECK(!decode_inventory_consume(bytes, heard));
}

// An item type's icon fitted to the game's 64 x 64.
KCD2S_TEST(inventory, item_icon_fitted) {
  // An uncompressed 32-bit .dds (A8R8G8B8), filled with one colour.
  auto uncompressed = [](std::uint32_t width, std::uint32_t height, std::uint32_t colour) {
    std::vector<std::uint8_t> bytes(128, 0);
    std::memcpy(bytes.data(), "DDS ", 4);
    const std::uint32_t fields[][2] = {
        {4, 124}, {12, height},     {16, width},      {76, 32},          {80, 0x41},
        {88, 32}, {92, 0x00FF0000}, {96, 0x0000FF00}, {100, 0x000000FF}, {104, 0xFF000000}};
    for (const auto& [at, value] : fields) {
      std::memcpy(bytes.data() + at, &value, 4);
    }
    for (std::uint32_t pixel = 0; pixel < width * height; ++pixel) {
      for (int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(std::uint8_t(colour >> shift));
      }
    }
    return bytes;
  };
  const auto big = uncompressed(256, 128, 0x80102030);
  const auto fitted = fit_item_icon(big);
  DdsPicture picture;
  KCD2S_CHECK(read_dds(fitted, picture) && picture.width == 64 && picture.height == 64 &&
              picture.rgba32);
  KCD2S_CHECK(fitted.size() == 128 + 64 * 64 * 4);
  // The colour survives the averaging: B, G, R, A of 0x80102030.
  KCD2S_CHECK(fitted[128] == 0x30 && fitted[129] == 0x20 && fitted[130] == 0x10 &&
              fitted[131] == 0x80);
  // A small one is scaled up too; a 64 x 64 one is kept as it is.
  KCD2S_CHECK(fit_item_icon(uncompressed(16, 16, 0xFF000000)).size() == 128 + 64 * 64 * 4);
  const auto exact = uncompressed(64, 64, 0xFFFFFFFF);
  KCD2S_CHECK(fit_item_icon(exact) == exact);
  // A compressed one of another size, a truncated one, and not a .dds: refused.
  auto compressed = uncompressed(1, 1, 0);
  const std::uint32_t four_cc = 0x4, dxt5 = 0x35545844, size = 128;
  std::memcpy(compressed.data() + 80, &four_cc, 4);
  std::memcpy(compressed.data() + 84, &dxt5, 4);
  std::memcpy(compressed.data() + 12, &size, 4);
  std::memcpy(compressed.data() + 16, &size, 4);
  KCD2S_CHECK(fit_item_icon(compressed).empty());
  auto truncated = big;
  truncated.resize(1000);
  KCD2S_CHECK(fit_item_icon(truncated).empty());
  KCD2S_CHECK(fit_item_icon({'P', 'N', 'G'}).empty());
  // A blip's map icon: 64 x 64 with its seven levels down to 1 x 1, the colour kept
  // through the levels; a compressed 64 x 64 one as it is.
  const auto icon = map_icon(big);
  std::uint32_t mips = 0;
  std::memcpy(&mips, icon.data() + 28, 4);
  KCD2S_CHECK(read_dds(icon, picture) && picture.width == 64 && picture.height == 64);
  KCD2S_CHECK(mips == 7);
  KCD2S_CHECK(icon.size() == 128 + (64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1) * 4);
  KCD2S_CHECK(icon[icon.size() - 4] == 0x30 && icon[icon.size() - 3] == 0x20 &&
              icon[icon.size() - 2] == 0x10 && icon[icon.size() - 1] == 0x80);
  auto compressed64 = compressed;
  const std::uint32_t side = 64;
  std::memcpy(compressed64.data() + 12, &side, 4);
  std::memcpy(compressed64.data() + 16, &side, 4);
  KCD2S_CHECK(map_icon(compressed64) == compressed64);
  KCD2S_CHECK(map_icon(compressed).empty());
  KCD2S_CHECK(transparent_icon(32).size() == 128 + 32 * 32 * 4);
}

KCD2S_TEST(inventory, world_items_round_trip) {
  using namespace kcd2s::integration;
  // A class's 16 bytes as the game keeps them; the key spells it as scripts do.
  std::uint8_t guid[16];
  for (int i = 0; i < 16; ++i) {
    guid[i] = std::uint8_t(i + 1);
  }
  const float position[3] = {1647.554f, -2.004f, 35.1f};
  const std::string key = world_item_key(guid, position);
  KCD2S_CHECK(key == "0807060504030201100f0e0d0c0b0a09@164755,-200,3510");
  KCD2S_CHECK(valid_world_item_key(key));
  KCD2S_CHECK(!valid_world_item_key("0807060504030201100f0e0d0c0b0a09@1,2"));
  KCD2S_CHECK(!valid_world_item_key("0807060504030201100F0E0D0C0B0A09@1,2,3"));
  std::string back;
  KCD2S_CHECK(decode_world_pickup(encode_world_pickup(key), back) && back == key);
  KCD2S_CHECK(encode_world_pickup("nope").empty());
  bool granted = false;
  KCD2S_CHECK(decode_world_pickup_verdict(encode_world_pickup_verdict(key, true), back, granted) &&
              back == key && granted);
  WorldTaken taken{true, {key, "0807060504030201100f0e0d0c0b0a09@0,0,0"}};
  WorldTaken taken_back;
  KCD2S_CHECK(decode_world_taken(encode_world_taken(taken), taken_back) && taken_back == taken);
  WorldTaken too_many;
  too_many.keys.assign(kMaxWorldTakenKeys + 1, key);
  KCD2S_CHECK(encode_world_taken(too_many).empty());
}

KCD2S_TEST(inventory, item_notice_round_trip) {
  using namespace kcd2s::integration;
  ItemNotice notice;
  notice.item[0] = 0x41;
  notice.item[15] = 0x9b;
  notice.amount = 20;
  ItemNotice back;
  KCD2S_CHECK(decode_item_notice(encode_item_notice(notice), back) && back == notice);
  notice.gained = false;
  KCD2S_CHECK(decode_item_notice(encode_item_notice(notice), back) && back == notice);
  // No units, or more than an inventory holds: nothing to show.
  ItemNotice none = notice;
  none.amount = 0;
  KCD2S_CHECK(encode_item_notice(none).empty());
  none.amount = kMaxItemNoticeAmount + 1;
  KCD2S_CHECK(encode_item_notice(none).empty());
}

KCD2S_TEST(inventory, shots_round_trip) {
  using namespace kcd2s::integration;
  Shot shot;
  shot.ammo[0] = 0x2f;
  shot.ammo[15] = 0x9a;
  shot.position[0] = 1647.51f;
  shot.position[1] = 2121.22f;
  shot.position[2] = 32.47f;
  shot.direction[1] = -0.9998f;
  shot.direction[2] = -0.019f;
  shot.speed = 46.81f;
  Shot back;
  KCD2S_CHECK(decode_shot(encode_shot(shot), back) && back == shot);
  std::uint32_t shooter = 0;
  KCD2S_CHECK(decode_shot_relay(encode_shot_relay(4098, shot), shooter, back) && shooter == 4098 &&
              back == shot);
  // A direction far from unit length, a speed no bow has, a shooter of 0: they do not travel.
  Shot bent = shot;
  bent.direction[1] = -3.0f;
  KCD2S_CHECK(encode_shot(bent).empty());
  Shot fast = shot;
  fast.speed = kMaxShotSpeed * 2;
  KCD2S_CHECK(encode_shot(fast).empty());
  KCD2S_CHECK(encode_shot_relay(0, shot).empty());
  // An NPC's shot names the NPC; one of no NPC does not travel, and the tags do not mix.
  std::uint32_t npc = 0;
  KCD2S_CHECK(decode_npc_shot(encode_npc_shot(4114, shot), npc, back) && npc == 4114 &&
              back == shot);
  KCD2S_CHECK(encode_npc_shot(0, shot).empty());
  KCD2S_CHECK(!decode_shot_relay(encode_npc_shot(4114, shot), shooter, back));
  // Where a projectile landed: its class, position and rotation; none outside the world or without
  // a rotation.
  ShotLanded landed;
  landed.ammo = shot.ammo;
  landed.position[0] = 1648.96f;
  landed.position[1] = 2127.44f;
  landed.position[2] = 31.43f;
  landed.rotation[0] = 0.5f;
  landed.rotation[1] = 0.5f;
  landed.rotation[2] = 0.5f;
  landed.rotation[3] = 0.5f;
  ShotLanded landed_back;
  KCD2S_CHECK(decode_shot_landed(encode_shot_landed(landed), landed_back) && landed_back == landed);
  // An arrow stuck in a target says so; a flag other than 0 or 1 is refused.
  ShotLanded stuck = landed;
  stuck.stuck = true;
  KCD2S_CHECK(decode_shot_landed(encode_shot_landed(stuck), landed_back) && landed_back.stuck);
  auto bad_flag = encode_shot_landed(stuck);
  bad_flag.back() = 2;
  KCD2S_CHECK(!decode_shot_landed(bad_flag, landed_back));
  // What it struck travels with it, as a raycast hit.
  ShotLanded struck = stuck;
  WorldRayHit target;
  target.position[0] = 1648.96f;
  target.normal[2] = 1.0f;
  target.distance = 0.4f;
  target.kind = RayHitKind::Entity;
  target.surface = "mat_wood";
  target.guid = "00ab00cd00ef0012";
  target.name = "archery_target_1";
  target.entity_class = "GeomEntity";
  struck.hit = target;
  KCD2S_CHECK(decode_shot_landed(encode_shot_landed(struck), landed_back) && landed_back == struck);
  ShotLanded far = landed;
  far.position[0] = kMaxShotCoordinate * 2;
  KCD2S_CHECK(encode_shot_landed(far).empty());
  ShotLanded flat = landed;
  flat.rotation[3] = 0;
  flat.rotation[0] = 0;
  KCD2S_CHECK(encode_shot_landed(flat).empty());
}

// What the server changes of the game's own items: ITEM_OVERRIDES.
KCD2S_TEST(inventory, item_overrides_round_trip) {
  ItemOverrides overrides;
  ItemOverride apple;
  parse_guid("2264f217-590e-4c0f-a4c6-f50c6532b9f6", apple.item);
  apple.has_weight = true;
  apple.weight = 1.5f;
  apple.has_price = true;
  apple.price = 20;
  ItemOverride bread;
  parse_guid("86e4ff24-88db-4024-abe6-46545fa0fbd1", bread.item);
  bread.has_price = true;
  bread.price = kMaxItemOverridePrice;
  bread.has_max = true;
  bread.max = 5;
  ItemOverride sword;
  parse_guid("00cca9e3-8ef2-46db-8cbf-86ec51930919", sword.item);
  sword.stats = {{std::uint8_t(ItemStat::kWeaponAttack), 200.0f},
                 {std::uint8_t(ItemStat::kWeaponAttackModStab), 1.5f}};
  overrides.items = {apple, bread, sword};
  overrides.max_tooltip = "Most you can carry";
  overrides.equipped_weight_percent = 25;
  ItemOverrides decoded;
  KCD2S_CHECK(decode_item_overrides(encode_item_overrides(overrides), decoded) &&
              decoded == overrides);
  // The equipped share is a percent.
  ItemOverrides heavy = overrides;
  heavy.equipped_weight_percent = 101;
  KCD2S_CHECK(encode_item_overrides(heavy).empty());
  // The empty set gives every class the game's own values back.
  KCD2S_CHECK(decode_item_overrides(encode_item_overrides(ItemOverrides{}), decoded) &&
              decoded.items.empty());
  // An override that changes nothing, or out of range, does not travel.
  ItemOverrides wrong = overrides;
  wrong.items[1].has_price = false;
  wrong.items[1].has_max = false;
  KCD2S_CHECK(encode_item_overrides(wrong).empty());
  wrong = overrides;
  wrong.items[0].weight = -1.0f;
  KCD2S_CHECK(encode_item_overrides(wrong).empty());
  wrong.items[0].weight = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(encode_item_overrides(wrong).empty());
  wrong = overrides;
  wrong.items[1].price = kMaxItemOverridePrice + 1;
  KCD2S_CHECK(encode_item_overrides(wrong).empty());
  wrong = overrides;
  wrong.items[1].max = 0;
  KCD2S_CHECK(encode_item_overrides(wrong).empty());
  wrong = overrides;
  wrong.max_tooltip = std::string(kMaxItemLimitTooltipBytes + 1, 'x');
  KCD2S_CHECK(encode_item_overrides(wrong).empty());
  wrong = overrides;
  wrong.items[2].stats[1].value = std::numeric_limits<float>::infinity();
  KCD2S_CHECK(encode_item_overrides(wrong).empty());
  wrong.items[2].stats.assign(kMaxItemOverrideStats + 1, {1, 1.0f});
  KCD2S_CHECK(encode_item_overrides(wrong).empty());
  // The notice of a limit the player reached.
  ItemLimitNotice notice{"You can't carry more than 5 of this item", false};
  ItemLimitNotice heard;
  KCD2S_CHECK(decode_item_limit_notice(encode_item_limit_notice(notice), heard) && heard == notice);
  notice.center = true;
  KCD2S_CHECK(decode_item_limit_notice(encode_item_limit_notice(notice), heard) && heard == notice);
  KCD2S_CHECK(heard.center);
  KCD2S_CHECK(encode_item_limit_notice(ItemLimitNotice{}).empty());
  KCD2S_CHECK(encode_item_limit_notice(
                  ItemLimitNotice{std::string(kMaxItemLimitNoticeBytes + 1, 'x'), false})
                  .empty());
  // Unknown flags and trailing bytes are refused.
  auto message = encode_item_overrides(overrides);
  message[2 + 16] = 16;
  KCD2S_CHECK(!decode_item_overrides(message, decoded));
  message = encode_item_overrides(overrides);
  message.push_back(0);
  KCD2S_CHECK(!decode_item_overrides(message, decoded));
}
