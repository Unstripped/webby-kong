"""tools/next_version.py: release version rule and its git integration."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from next_version import next_version  # noqa: E402


class NextVersionRuleTest(unittest.TestCase):
    def test_first_release(self):
        self.assertEqual(next_version([], []), "v0.1.0")

    def test_patch_by_default(self):
        self.assertEqual(next_version(["v0.1.0"], ["web: x"]), "v0.1.1")

    def test_minor_marker_in_body(self):
        self.assertEqual(next_version(["v0.1.0"], ["gfx: y\n\n#minor"]), "v0.2.0")

    def test_major_beats_minor(self):
        self.assertEqual(next_version(["v0.3.4"], ["a #minor", "b #major"]), "v1.0.0")

    def test_numeric_order_and_foreign_tags_ignored(self):
        self.assertEqual(next_version(["v0.1.9", "v0.1.10", "nightly", "v1"], []), "v0.1.11")


class NextVersionGitTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self.git("init", "-q", "-b", "main")
        self.commit("first")

    def tearDown(self):
        self._tmp.cleanup()

    def git(self, *args):
        subprocess.run(["git", "-c", "user.name=test", "-c", "user.email=test@example.invalid", *args],
                       cwd=self.repo, check=True)

    def commit(self, message):
        self.git("commit", "-q", "--allow-empty", "-m", message)

    def run_script(self, *args):
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "next_version.py"), *args],
                           cwd=self.repo, capture_output=True, text=True, check=True)
        return r.stdout.strip()

    def test_release_sequence(self):
        self.assertEqual(self.run_script(), "v0.1.0")
        self.git("tag", "v0.1.0")
        self.assertEqual(self.run_script(), "")
        self.commit("x #minor")
        self.assertEqual(self.run_script(), "v0.2.0")
        self.assertEqual(self.run_script("--previous"), "v0.1.0")


if __name__ == "__main__":
    unittest.main()
