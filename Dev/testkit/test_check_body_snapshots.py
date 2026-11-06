"""Evidence integrity checks; these fixtures do not validate native gameplay."""

import copy
import unittest

from check_body_snapshots import compare, latest_snapshot


def sample(owner, tick=1001):
    lines = [
        f"BODY SNAPSHOT begin sample=1 pid={100 + owner} revision=4 owner={owner} "
        f"rows=3 truncated=0 tick={tick}"
    ]
    for player in (1, 2, 3):
        lines.append(
            f"BODY SNAPSHOT row sample=1 player={player} owner={int(player == owner)} "
            f"available=1 hp=100.000000 native_ko=0 profile=1 action=0 tick={tick + 1}"
        )
    lines.append(f"BODY SNAPSHOT end sample=1 rows=3 tick={tick + 2}")
    return lines


class BodySnapshotChecks(unittest.TestCase):
    def setUp(self):
        self.snapshots = [latest_snapshot(sample(owner)) for owner in (1, 2, 3)]

    def test_complete_current_views_agree_without_claiming_visual_acceptance(self):
        result = compare(self.snapshots, 1000)
        self.assertTrue(result["hp_and_native_ko_agree"])
        self.assertFalse(result["visual_acceptance"])

    def test_immortal_knockout_is_distinct_from_positive_health_awake(self):
        for snapshot in self.snapshots:
            snapshot["bodies"][1]["hp"] = 1.0
            snapshot["bodies"][1]["native_ko"] = 1
        self.assertTrue(compare(self.snapshots, 1000)["hp_and_native_ko_agree"])
        self.snapshots[1]["bodies"][1]["native_ko"] = 0
        self.assertFalse(compare(self.snapshots, 1000)["hp_and_native_ko_agree"])

    def test_health_disagreement_fails_even_when_knockout_agrees(self):
        self.snapshots[2]["bodies"][1]["hp"] = 90.0
        self.assertFalse(compare(self.snapshots, 1000)["hp_and_native_ko_agree"])

    def test_latest_partial_snapshot_cannot_fall_back_to_older_success(self):
        lines = sample(1) + [
            "BODY SNAPSHOT begin sample=2 pid=101 revision=4 owner=1 rows=3 truncated=0 tick=2000"
        ]
        with self.assertRaises(ValueError):
            latest_snapshot(lines)

    def test_missing_and_duplicate_rows_are_rejected(self):
        missing = sample(1)
        missing.pop(2)
        duplicate = sample(1)
        duplicate.insert(2, duplicate[1])
        for lines in (missing, duplicate):
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                latest_snapshot(lines)

    def test_stale_or_different_world_samples_are_rejected(self):
        with self.assertRaises(ValueError):
            compare(self.snapshots, 1001)
        self.snapshots[1]["revision"] = 5
        with self.assertRaises(ValueError):
            compare(self.snapshots, 1000)

    def test_reused_process_or_owner_cannot_count_as_three_views(self):
        for field in ("pid", "owner"):
            snapshots = copy.deepcopy(self.snapshots)
            snapshots[1][field] = snapshots[0][field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                compare(snapshots, 1000)

    def test_truncated_or_unavailable_observation_is_not_agreement(self):
        self.snapshots[0]["truncated"] = 1
        with self.assertRaises(ValueError):
            compare(self.snapshots, 1000)
        self.snapshots[0]["truncated"] = 0
        self.snapshots[0]["bodies"][1]["available"] = 0
        with self.assertRaises(ValueError):
            compare(self.snapshots, 1000)

    def test_missing_player_and_wrong_owner_marker_are_rejected(self):
        missing = copy.deepcopy(self.snapshots)
        del missing[1]["bodies"][3]
        with self.assertRaises(ValueError):
            compare(missing, 1000)
        self.snapshots[0]["bodies"][1]["owner"] = 0
        with self.assertRaises(ValueError):
            compare(self.snapshots, 1000)

    def test_nonfinite_nonpositive_health_and_invalid_ko_are_rejected(self):
        for field, value in (("hp", float("nan")), ("hp", 0), ("native_ko", 2)):
            snapshots = copy.deepcopy(self.snapshots)
            snapshots[0]["bodies"][1][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                compare(snapshots, 1000)

    def test_observation_skew_and_row_timestamps_are_checked(self):
        self.snapshots[2] = latest_snapshot(sample(3, 1200))
        with self.assertRaises(ValueError):
            compare(self.snapshots, 1000)
        self.snapshots[2] = latest_snapshot(sample(3))
        self.snapshots[1]["bodies"][2]["tick"] = 999
        with self.assertRaises(ValueError):
            compare(self.snapshots, 1000)

    def test_body_profiles_are_reported_separately_from_native_ko(self):
        self.snapshots[1]["bodies"][1]["profile"] = 2
        result = compare(self.snapshots, 1000)
        self.assertTrue(result["hp_and_native_ko_agree"])
        self.assertEqual(result["players"][0]["profiles"], [1, 2, 1])
        self.assertFalse(result["visual_acceptance"])


if __name__ == "__main__":
    unittest.main()
