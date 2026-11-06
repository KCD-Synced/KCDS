#include "clinch_handshake.hpp"
#include "kcd2s/testkit.hpp"

using kcds::runtime::combat::ClinchAnswer;
using kcds::runtime::combat::ClinchHandshake;
using kcds::runtime::combat::kApproachClinch;
using kcds::runtime::combat::kClinchAcceptWindowMs;
using kcds::runtime::combat::kClinchRequestRepeatMs;
using kcds::runtime::combat::kGuardClinch;

namespace {
constexpr std::uint32_t kLow = 4;
constexpr std::uint32_t kHigh = 5;
}  // namespace

KCD2S_TEST(clinch_handshake, a_request_is_asked_again_only_after_the_repeat_time) {
  ClinchHandshake handshake;
  KCD2S_CHECK(handshake.request(kHigh, kApproachClinch, 1000));
  KCD2S_CHECK(!handshake.request(kHigh, kApproachClinch, 1000 + kClinchRequestRepeatMs));
  KCD2S_CHECK(handshake.request(kHigh, kApproachClinch, 1001 + kClinchRequestRepeatMs));
  // Another player is asked independently.
  KCD2S_CHECK(handshake.request(kLow, kApproachClinch, 1002 + kClinchRequestRepeatMs));
}

KCD2S_TEST(clinch_handshake, when_both_ask_at_once_the_lower_player_id_initiates) {
  // Live: both clients saw the approach within 15 ms, before either request arrived.
  ClinchHandshake low;
  ClinchHandshake high;
  KCD2S_CHECK(low.request(kHigh, kApproachClinch, 1000));
  KCD2S_CHECK(high.request(kLow, kApproachClinch, 1015));
  // The lower id's client keeps its own request; the higher id's client starts the other's.
  KCD2S_CHECK(low.answer(kLow, kHigh, true, 1050) == ClinchAnswer::Ignore);
  KCD2S_CHECK(high.answer(kHigh, kLow, true, 1050) == ClinchAnswer::Start);
  // The higher id's own request is overtaken: an accept for it no longer counts.
  KCD2S_CHECK(!high.accepted(kLow, kApproachClinch, 1100));
  KCD2S_CHECK(low.accepted(kHigh, kApproachClinch, 1100));
  KCD2S_CHECK(!low.accepted(kHigh, kApproachClinch, 1101));
}

KCD2S_TEST(clinch_handshake, an_old_own_request_does_not_win_against_a_new_one) {
  ClinchHandshake low;
  KCD2S_CHECK(low.request(kHigh, kApproachClinch, 1000));
  KCD2S_CHECK(low.answer(kLow, kHigh, true, 1001 + kClinchAcceptWindowMs) == ClinchAnswer::Start);
}

KCD2S_TEST(clinch_handshake, a_request_that_cannot_start_is_refused_at_once) {
  // Live run 2: a request that waited for the other client's own start was renewed every
  // 500 ms for 20 s and never started. Now every request is answered when it arrives.
  ClinchHandshake asked;
  for (std::uint64_t now = 1000; now < 21000; now += 500) {
    KCD2S_CHECK(asked.answer(kLow, kHigh, false, now) == ClinchAnswer::Refuse);
  }
  KCD2S_CHECK(asked.answer(kLow, kHigh, true, 21000) == ClinchAnswer::Start);
}

KCD2S_TEST(clinch_handshake, a_refusal_answers_the_request_and_a_late_accept_is_ignored) {
  ClinchHandshake asking;
  KCD2S_CHECK(!asking.refused(kHigh, kApproachClinch));
  KCD2S_CHECK(asking.request(kHigh, kApproachClinch, 1000));
  KCD2S_CHECK(!asking.refused(kHigh, kGuardClinch));
  KCD2S_CHECK(!asking.refused(kLow, kApproachClinch));
  KCD2S_CHECK(asking.refused(kHigh, kApproachClinch));
  KCD2S_CHECK(!asking.refused(kHigh, kApproachClinch));
  KCD2S_CHECK(!asking.accepted(kHigh, kApproachClinch, 1100));
  // A refused request no longer wins a tie either.
  KCD2S_CHECK(asking.answer(kLow, kHigh, true, 1200) == ClinchAnswer::Start);
  // The game asks again only after the repeat time.
  KCD2S_CHECK(!asking.request(kHigh, kApproachClinch, 1200));
  KCD2S_CHECK(asking.request(kHigh, kApproachClinch, 1501));
}

KCD2S_TEST(clinch_handshake, an_accept_answers_only_the_fresh_request_of_its_kind_once) {
  ClinchHandshake handshake;
  KCD2S_CHECK(!handshake.accepted(kHigh, kApproachClinch, 1000));
  KCD2S_CHECK(handshake.request(kHigh, kApproachClinch, 1000));
  KCD2S_CHECK(!handshake.accepted(kHigh, kGuardClinch, 1100));
  KCD2S_CHECK(!handshake.accepted(kLow, kApproachClinch, 1100));
  KCD2S_CHECK(handshake.accepted(kHigh, kApproachClinch, 1000 + kClinchAcceptWindowMs));
  KCD2S_CHECK(!handshake.accepted(kHigh, kApproachClinch, 1000 + kClinchAcceptWindowMs));
  KCD2S_CHECK(handshake.request(kHigh, kApproachClinch, 3000));
  KCD2S_CHECK(!handshake.accepted(kHigh, kApproachClinch, 3001 + kClinchAcceptWindowMs));
}

KCD2S_TEST(clinch_handshake, a_gone_replica_or_a_new_world_drops_its_requests) {
  ClinchHandshake handshake;
  KCD2S_CHECK(handshake.request(kHigh, kApproachClinch, 1000));
  handshake.forget(kHigh);
  KCD2S_CHECK(!handshake.accepted(kHigh, kApproachClinch, 1100));
  KCD2S_CHECK(handshake.request(kHigh, kApproachClinch, 1100));
  KCD2S_CHECK(handshake.request(kLow, kApproachClinch, 1100));
  handshake.reset();
  KCD2S_CHECK(!handshake.accepted(kHigh, kApproachClinch, 1200));
  KCD2S_CHECK(!handshake.accepted(kLow, kApproachClinch, 1200));
  KCD2S_CHECK(handshake.request(kHigh, kApproachClinch, 1200));
}
