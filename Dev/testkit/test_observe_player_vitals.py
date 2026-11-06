"""Checks for log identity fencing and the three-view HP acceptance observer."""

import unittest

from observe_player_vitals import Agreement, ClientLog, KnockoutEvidence


class IdentityTests(unittest.TestCase):
    def test_requires_observed_owner_and_engine_ids(self):
        log = ClientLog("client-123.log")
        with self.assertRaises(ValueError):
            log.actors()
        log.accept("[10] READINESS WH player=ABC entity=DEF id=0x1234")
        log.accept("[20] FORCE_XFORM applied player=7 epoch=2")
        log.accept("[30] REMOTE player=8 engine=0x5678 class=Player channel=0")
        self.assertEqual(log.actors(), {7: 0x1234, 8: 0x5678})
        log.accept("[40] KNOCKOUT replica of player=8 down (unconsciousness=1, buffs=3)")
        self.assertTrue(log.down[8])
        self.assertEqual(log.down_at[8], 40)
        log.accept("[50] REMOTE removed player=8")
        self.assertEqual(log.actors(), {7: 0x1234})
        self.assertNotIn(8, log.down)
        self.assertNotIn(8, log.down_at)


class AgreementTests(unittest.TestCase):
    def test_transient_lag_is_exposed_but_settled_agreement_passes(self):
        agreement = Agreement(300)
        agreement.observe(1000, 7, 100, [100, 100])
        agreement.observe(1010, 7, 25, [100, 100])
        agreement.observe(1100, 7, 25, [25, 25])
        agreement.observe(1400, 7, 25.004, [25, 25.01])
        result = agreement.result()
        self.assertTrue(result["hp_agreement_passed"])
        self.assertEqual(result["transient_mismatches"], 2)
        self.assertFalse(result["native_lifecycle_proven"])

    def test_positive_owner_with_zero_replica_fails_even_before_settling(self):
        agreement = Agreement(300)
        agreement.observe(1000, 7, 1, [0, 1])
        agreement.observe(1400, 7, 1, [1, 1])
        self.assertFalse(agreement.result()["hp_agreement_passed"])
        self.assertEqual(agreement.zero_replica_alive_owner, 1)

    def test_settled_mismatch_and_missing_view_fail(self):
        agreement = Agreement(300)
        agreement.observe(1000, 7, 50, [50, 50])
        agreement.observe(1400, 7, 50, [49.98])
        result = agreement.result()
        self.assertFalse(result["hp_agreement_passed"])
        self.assertEqual(result["settled_mismatches"], 1)
        self.assertEqual(result["missing_views"], 1)

    def test_continuously_changing_owner_does_not_prove_settled_agreement(self):
        agreement = Agreement(300)
        for tick in range(1000, 2000, 20):
            value = tick / 100
            agreement.observe(tick, 7, value, [value, value])
        self.assertFalse(agreement.result()["hp_agreement_passed"])

    def test_each_owner_needs_a_settled_comparison(self):
        agreement = Agreement(300)
        for tick in range(1000, 2000, 20):
            agreement.observe(tick, 7, 100, [100, 100])
            changing = tick / 100
            agreement.observe(tick, 8, changing, [changing, changing])
        self.assertGreater(agreement.comparisons, 0)
        self.assertFalse(agreement.result()["hp_agreement_passed"])

    def test_initial_agreement_does_not_cover_dense_unsettled_damage_at_the_end(self):
        agreement = Agreement(300)
        agreement.observe(1000, 7, 100, [100, 100])
        agreement.observe(1400, 7, 100, [100, 100])
        for tick in range(1500, 2000, 20):
            agreement.observe(tick, 7, 100 - (tick - 1500) / 10, [100, 100])
        result = agreement.result()
        self.assertGreater(result["comparisons"], 0)
        self.assertFalse(result["hp_agreement_passed"])
        self.assertEqual(result["unsettled_final_owners"], [7])
        self.assertEqual(result["final_mismatch_owners"], [7])


class KnockoutEvidenceTests(unittest.TestCase):
    def test_healing_without_revive_does_not_change_observed_knockout_intent(self):
        evidence = KnockoutEvidence(300)
        # HP is deliberately absent: either low or healed positive HP can accompany a KO.
        evidence.observe(2000, 7, True, 1000, [True, True])
        self.assertEqual(evidence.result()["settled_intent_buff_mismatches"], 0)
        self.assertEqual(evidence.result()["last_known_owner_intents"], {7: True})

    def test_stale_replica_knockout_after_revive_is_exposed_after_settling(self):
        evidence = KnockoutEvidence(300)
        evidence.observe(1100, 7, False, 1000, [True, False])
        self.assertEqual(evidence.mismatches, 0)
        evidence.observe(1400, 7, False, 1000, [True, False])
        self.assertEqual(evidence.mismatches, 1)
        self.assertFalse(evidence.result()["native_lifecycle_proven"])

    def test_unknown_owner_or_buff_does_not_become_false_agreement(self):
        evidence = KnockoutEvidence(300)
        evidence.observe(2000, 7, None, None, [False, False])
        evidence.observe(2000, 8, True, 1000, [None, True])
        self.assertEqual(evidence.unknown_owner_samples, 1)
        self.assertEqual(evidence.unknown_replica_samples, 1)


if __name__ == "__main__":
    unittest.main()
