#include "damage_reports.hpp"

#include <cstdint>
#include <optional>

#include "kcd2s/testkit.hpp"

namespace {

using kcd2s::integration::PlayerDamage;
using kcds::runtime::combat::DamageReports;
using kcds::runtime::combat::quiet_damage_eligible;

PlayerDamage hit(std::uint32_t attacker, float amount = 5.0f, std::uint8_t reason = 1) {
  PlayerDamage damage;
  damage.attacker = attacker;
  damage.amount = amount;
  damage.reason = reason;
  return damage;
}

}  // namespace

KCD2S_TEST(damage_reports, quiet_tick_before_lethal_sword_in_same_millisecond_is_not_the_killer) {
  constexpr std::uint64_t occurrence = 5000;
  constexpr std::uint64_t quiet_receipt = 10;
  constexpr std::uint64_t sword_receipt = 11;
  KCD2S_CHECK(!quiet_damage_eligible(occurrence, occurrence, quiet_receipt, sword_receipt));
}

KCD2S_TEST(damage_reports, quiet_tick_after_sword_in_same_millisecond_can_be_the_killer) {
  constexpr std::uint64_t occurrence = 5000;
  constexpr std::uint64_t sword_receipt = 10;
  constexpr std::uint64_t quiet_receipt = 11;
  KCD2S_CHECK(quiet_damage_eligible(occurrence, occurrence, quiet_receipt, sword_receipt));
}

KCD2S_TEST(damage_reports, quiet_killing_history_expires_after_the_inclusive_one_second_boundary) {
  constexpr std::uint64_t occurred = 5000;
  KCD2S_CHECK(quiet_damage_eligible(occurred + 999, occurred, 2, 1));
  KCD2S_CHECK(quiet_damage_eligible(occurred + 1000, occurred, 2, 1));
  KCD2S_CHECK(!quiet_damage_eligible(occurred + 1001, occurred, 2, 1));
  KCD2S_CHECK(!quiet_damage_eligible(occurred - 1, occurred, 2, 1));
}

KCD2S_TEST(damage_reports, quiet_killing_history_requires_a_recorded_unique_receipt) {
  KCD2S_CHECK(!quiet_damage_eligible(100, 0, 2, 1));
  KCD2S_CHECK(!quiet_damage_eligible(100, 100, 0, 0));
  KCD2S_CHECK(!quiet_damage_eligible(100, 100, 2, 2));
  KCD2S_CHECK(quiet_damage_eligible(100, 100, 1, 0));
}

KCD2S_TEST(damage_reports, delayed_missile_resolution_retains_its_original_quiet_history_order) {
  constexpr std::uint64_t arrow_receipt = 10;
  constexpr std::uint64_t quiet_receipt = 11;
  // The arrow resolves after the quiet tick. Its original receipt, rather than the time of
  // resolution, determines whether the later quiet tick can be reported as killing damage.
  KCD2S_CHECK(quiet_damage_eligible(5500, 5000, quiet_receipt, arrow_receipt));
  KCD2S_CHECK(!quiet_damage_eligible(5500, 5000, arrow_receipt, quiet_receipt));
}

KCD2S_TEST(damage_reports, held_arrow_precedes_later_sword_across_frames) {
  DamageReports reports;
  const auto arrow = reports.stage_pending(hit(7), 100);
  KCD2S_CHECK(arrow.has_value());
  if (!arrow) {
    return;
  }
  KCD2S_CHECK(reports.push_ready(hit(8, 40.0f), 120));
  for (unsigned frame = 0; frame < 4; ++frame) {
    KCD2S_CHECK(reports.take_ready().empty());
    KCD2S_CHECK_EQ(reports.size(), std::size_t{2});
  }
  KCD2S_CHECK(reports.resolve_original(*arrow));
  const auto ready = reports.take_ready();
  KCD2S_CHECK_EQ(ready.size(), std::size_t{2});
  if (ready.size() != 2) {
    return;
  }
  KCD2S_CHECK_EQ(ready[0].damage.attacker, std::uint32_t{7});
  KCD2S_CHECK_EQ(ready[1].damage.attacker, std::uint32_t{8});
  KCD2S_CHECK_EQ(ready[0].occurrence, std::uint64_t{100});
  KCD2S_CHECK_EQ(ready[1].occurrence, std::uint64_t{120});
  KCD2S_CHECK_EQ(reports.pending(), std::size_t{0});
  KCD2S_CHECK(reports.take_ready().empty());
}

KCD2S_TEST(damage_reports, drains_only_prefix_before_the_next_unresolved_report) {
  DamageReports reports;
  KCD2S_CHECK(reports.push_ready(hit(1), 10));
  const auto first = reports.stage_pending(hit(2), 20);
  KCD2S_CHECK(first.has_value());
  if (!first) {
    return;
  }
  KCD2S_CHECK(reports.push_ready(hit(3), 30));
  const auto second = reports.stage_pending(hit(4), 40);
  KCD2S_CHECK(second.has_value());
  if (!second) {
    return;
  }
  KCD2S_CHECK(reports.push_ready(hit(5), 50));
  auto ready = reports.take_ready();
  KCD2S_CHECK_EQ(ready.size(), std::size_t{1});
  if (ready.size() != 1) {
    return;
  }
  KCD2S_CHECK_EQ(ready[0].damage.attacker, std::uint32_t{1});
  KCD2S_CHECK(reports.resolve_original(*first));
  ready = reports.take_ready();
  KCD2S_CHECK_EQ(ready.size(), std::size_t{2});
  if (ready.size() != 2) {
    return;
  }
  KCD2S_CHECK_EQ(ready[0].damage.attacker, std::uint32_t{2});
  KCD2S_CHECK_EQ(ready[1].damage.attacker, std::uint32_t{3});
  KCD2S_CHECK(reports.take_ready().empty());
  KCD2S_CHECK(reports.resolve_original(*second));
  ready = reports.take_ready();
  KCD2S_CHECK_EQ(ready.size(), std::size_t{2});
  if (ready.size() != 2) {
    return;
  }
  KCD2S_CHECK_EQ(ready[0].damage.attacker, std::uint32_t{4});
  KCD2S_CHECK_EQ(ready[1].damage.attacker, std::uint32_t{5});
}

KCD2S_TEST(damage_reports, out_of_order_rulings_do_not_reorder_native_receipt) {
  DamageReports reports;
  const auto first = reports.stage_pending(hit(1), 400);
  const auto second = reports.stage_pending(hit(2), 300);
  KCD2S_CHECK(first.has_value() && second.has_value());
  if (!first || !second) {
    return;
  }
  KCD2S_CHECK(reports.push_ready(hit(3), 200));
  KCD2S_CHECK(reports.resolve(*second, hit(2, 17.5f)));
  KCD2S_CHECK(reports.take_ready().empty());
  KCD2S_CHECK(reports.resolve_original(*first));
  const auto ready = reports.take_ready();
  KCD2S_CHECK_EQ(ready.size(), std::size_t{3});
  if (ready.size() != 3) {
    return;
  }
  KCD2S_CHECK_EQ(ready[0].damage.attacker, std::uint32_t{1});
  KCD2S_CHECK_EQ(ready[1].damage.attacker, std::uint32_t{2});
  KCD2S_CHECK_EQ(ready[1].damage.amount, 17.5f);
  KCD2S_CHECK_EQ(ready[1].occurrence, std::uint64_t{300});
  KCD2S_CHECK_EQ(ready[2].damage.attacker, std::uint32_t{3});
}

KCD2S_TEST(damage_reports, veto_and_failed_send_cancel_without_blocking_later_reports) {
  DamageReports reports;
  const auto refused = reports.stage_pending(hit(1), 10);
  const auto failed_send = reports.stage_pending(hit(2), 20);
  KCD2S_CHECK(refused.has_value() && failed_send.has_value());
  if (!refused || !failed_send) {
    return;
  }
  KCD2S_CHECK(reports.push_ready(hit(3), 30));
  KCD2S_CHECK(reports.resolve(*failed_send, std::nullopt));
  KCD2S_CHECK(reports.take_ready().empty());
  KCD2S_CHECK(reports.resolve(*refused, std::nullopt));
  const auto ready = reports.take_ready();
  KCD2S_CHECK_EQ(ready.size(), std::size_t{1});
  if (ready.size() != 1) {
    return;
  }
  KCD2S_CHECK_EQ(ready[0].damage.attacker, std::uint32_t{3});
  KCD2S_CHECK_EQ(reports.size(), std::size_t{0});
}

KCD2S_TEST(damage_reports, a_resolution_is_one_shot_even_before_the_report_drains) {
  DamageReports reports;
  const auto blocker = reports.stage_pending(hit(1), 10);
  const auto changed = reports.stage_pending(hit(2), 20);
  KCD2S_CHECK(blocker.has_value() && changed.has_value());
  if (!blocker || !changed) {
    return;
  }
  KCD2S_CHECK(reports.resolve(*changed, hit(2, 8.0f)));
  KCD2S_CHECK(!reports.resolve(*changed, hit(2, 99.0f)));
  KCD2S_CHECK(!reports.resolve_original(*changed));
  KCD2S_CHECK(!reports.resolve(0, std::nullopt));
  KCD2S_CHECK(!reports.resolve(999, hit(3)));
  KCD2S_CHECK(reports.resolve(*blocker, std::nullopt));
  const auto ready = reports.take_ready();
  KCD2S_CHECK_EQ(ready.size(), std::size_t{1});
  if (ready.size() != 1) {
    return;
  }
  KCD2S_CHECK_EQ(ready[0].damage.amount, 8.0f);
  KCD2S_CHECK(!reports.resolve_original(*changed));
}

KCD2S_TEST(damage_reports, clear_discards_all_states_and_late_rulings_cannot_touch_new_life) {
  DamageReports reports;
  const auto old = reports.stage_pending(hit(1), 10);
  const auto old_ready = reports.stage_pending(hit(2), 20);
  KCD2S_CHECK(old.has_value() && old_ready.has_value());
  if (!old || !old_ready) {
    return;
  }
  KCD2S_CHECK(reports.resolve_original(*old_ready));
  KCD2S_CHECK(reports.push_ready(hit(3), 30));
  reports.clear();
  KCD2S_CHECK_EQ(reports.size(), std::size_t{0});
  KCD2S_CHECK_EQ(reports.pending(), std::size_t{0});
  KCD2S_CHECK(reports.take_ready().empty());
  const auto current = reports.stage_pending(hit(4), 40);
  KCD2S_CHECK(current.has_value());
  if (!current) {
    return;
  }
  KCD2S_CHECK(*current > *old_ready);
  KCD2S_CHECK(!reports.resolve_original(*old));
  KCD2S_CHECK(!reports.resolve(*old_ready, hit(9)));
  KCD2S_CHECK(reports.take_ready().empty());
  KCD2S_CHECK(reports.resolve_original(*current));
  const auto ready = reports.take_ready();
  KCD2S_CHECK_EQ(ready.size(), std::size_t{1});
  if (ready.size() != 1) {
    return;
  }
  KCD2S_CHECK_EQ(ready[0].damage.attacker, std::uint32_t{4});
}

KCD2S_TEST(damage_reports, overflow_is_explicit_and_does_not_evict_the_unresolved_head) {
  DamageReports reports;
  const auto first = reports.stage_pending(hit(1), 10);
  KCD2S_CHECK(first.has_value());
  if (!first) {
    return;
  }
  for (std::size_t index = 1; index < DamageReports::kCapacity; ++index) {
    KCD2S_CHECK(reports.push_ready(hit(std::uint32_t(index + 1)), index));
  }
  KCD2S_CHECK_EQ(reports.size(), DamageReports::kCapacity);
  KCD2S_CHECK(!reports.push_ready(hit(999), 999));
  KCD2S_CHECK(!reports.stage_pending(hit(999), 999).has_value());
  KCD2S_CHECK(reports.take_ready().empty());
  KCD2S_CHECK(reports.resolve_original(*first));
  const auto ready = reports.take_ready();
  KCD2S_CHECK_EQ(ready.size(), DamageReports::kCapacity);
  if (ready.size() != DamageReports::kCapacity) {
    return;
  }
  for (std::size_t index = 0; index < ready.size(); ++index) {
    KCD2S_CHECK_EQ(ready[index].damage.attacker, std::uint32_t(index + 1));
  }
  KCD2S_CHECK(reports.push_ready(hit(1000), 1000));
  KCD2S_CHECK_EQ(reports.take_ready().size(), std::size_t{1});
}

KCD2S_TEST(damage_reports, ring_reuse_keeps_pending_barriers_and_original_scalar_metadata) {
  DamageReports reports;
  for (std::uint32_t round = 0; round < 150; ++round) {
    PlayerDamage original = hit(round + 1, 2.5f, 7);
    original.subpart = 24;
    const auto pending = reports.stage_pending(original, 1000 + round);
    KCD2S_CHECK(pending.has_value());
    if (!pending) {
      return;
    }
    KCD2S_CHECK(reports.push_ready(hit(500 + round), 2000 + round));
    KCD2S_CHECK(reports.take_ready().empty());
    original.attacker = 9999;
    KCD2S_CHECK(reports.resolve_original(*pending));
    const auto ready = reports.take_ready();
    KCD2S_CHECK_EQ(ready.size(), std::size_t{2});
    if (ready.size() != 2) {
      return;
    }
    KCD2S_CHECK_EQ(ready[0].damage.attacker, round + 1);
    KCD2S_CHECK_EQ(ready[0].damage.reason, std::uint8_t{7});
    KCD2S_CHECK_EQ(ready[0].damage.subpart, 24);
    KCD2S_CHECK_EQ(ready[0].occurrence, std::uint64_t{1000} + round);
    KCD2S_CHECK_EQ(ready[1].damage.attacker, 500 + round);
    KCD2S_CHECK_EQ(reports.size(), std::size_t{0});
  }
}

KCD2S_TEST(damage_reports, knockout_releases_committed_arrow_before_later_killing_damage) {
  DamageReports reports;
  const auto arrow = reports.stage_pending(hit(7, 3.0f), 100);
  KCD2S_CHECK(arrow.has_value());
  if (!arrow) {
    return;
  }
  KCD2S_CHECK(reports.push_ready(hit(8, 40.0f), 120));
  // KO retains the committed missile damage, clears the native held record, then sends damage
  // before the KO notification. Resolving its ticket must keep the sword last for attribution.
  KCD2S_CHECK(reports.resolve_original(*arrow));
  const auto ready = reports.take_ready();
  KCD2S_CHECK_EQ(ready.size(), std::size_t{2});
  if (ready.size() != 2) {
    return;
  }
  KCD2S_CHECK_EQ(ready.back().damage.attacker, std::uint32_t{8});
  KCD2S_CHECK_EQ(ready.front().occurrence, std::uint64_t{100});
}
