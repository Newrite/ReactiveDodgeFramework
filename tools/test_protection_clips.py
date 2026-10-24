import unittest

from migrate_protection_windows import migrate_notes


class ProtectionMigration(unittest.TestCase):
    def test_starts_move_to_frame_zero_and_ends_and_other_notes_stay(self):
        original = [(0.0, "RDF_Windows"), (0.125, "RDF_IFrameStart"),
                    (0.15, "RDF_PerfectStart"), (0.2, "RDF_PerfectEnd"),
                    (0.4, "RDF_IFrameEnd"), (0.5, "animmotion 1 2 3")]
        migrated = migrate_notes(original)
        self.assertEqual(migrated, [(0.0, "RDF_Windows"), (0.0, "RDF_IFrameStart.0.125000"),
                                   (0.0, "RDF_PerfectStart.0.150000"), *original[3:]])
        self.assertEqual(migrate_notes(migrated), migrated)

    def test_unannotated_clip_is_unchanged(self):
        original = [(0.1, "animmotion 0 1 0"), (0.4, "RDF_IFrameEnd")]
        self.assertEqual(migrate_notes(original), original)

    def test_ambiguous_or_invalid_metadata_is_rejected(self):
        for notes in [[(0.1, "RDF_IFrameStart"), (0.3, "RDF_IFrameStart")],
                      [(0.1, "RDF_PerfectStart.0.2")], [(0, "RDF_IFrameStart.nan")]]:
            with self.subTest(notes=notes), self.assertRaises(ValueError):
                migrate_notes(notes)


if __name__ == "__main__":
    unittest.main()
