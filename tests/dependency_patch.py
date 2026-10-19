"""Regression checks for the pinned, idempotent dependency patch contract."""

from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from patch_ftxui import PATCHES, patch


class DependencyPatchTests(unittest.TestCase):
    def setUp(self):
        cache = ROOT / ".cache"
        cache.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="ftxui-patch-", dir=cache)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "src/ftxui/component/app.cpp"
        self.source.parent.mkdir(parents=True)
        self.original = "\n".join(before for before, _ in PATCHES)
        self.source.write_text(self.original, encoding="utf-8")

    def test_applies_once_and_is_idempotent(self):
        self.assertTrue(patch(self.root))
        first = self.source.read_bytes()
        self.assertFalse(patch(self.root))
        self.assertEqual(first, self.source.read_bytes())
        updated = first.decode("utf-8")
        for _, after in PATCHES:
            self.assertEqual(updated.count(after), 1)

    def test_source_drift_does_not_partially_patch(self):
        self.source.write_text(self.original.replace("wstring.clear();", "wstring.clear( );"),
                               encoding="utf-8")
        before = self.source.read_bytes()
        with self.assertRaisesRegex(RuntimeError, "source context changed"):
            patch(self.root)
        self.assertEqual(before, self.source.read_bytes())

    def test_duplicate_context_is_rejected(self):
        self.source.write_text(self.original + PATCHES[0][0], encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "source context changed"):
            patch(self.root)


if __name__ == "__main__":
    unittest.main()
