"""tools/check_no_roms.py rejects ROMs and ROM-derived files staged in a git index."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "check_no_roms.py"


class CheckNoRomsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        subprocess.run(["git", "init", "-q", "-b", "main"], cwd=self.repo, check=True)
        (self.repo / "README.md").write_text("hello\n")
        self.stage("README.md")

    def tearDown(self):
        self._tmp.cleanup()

    def stage(self, name, data=None):
        if data is not None:
            (self.repo / name).write_bytes(data)
        subprocess.run(["git", "add", "-f", name], cwd=self.repo, check=True)

    def check(self):
        return subprocess.run([sys.executable, str(SCRIPT)], cwd=self.repo, capture_output=True, text=True)

    def assert_rejected(self, name, reason):
        r = self.check()
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn(f"{name}: {reason}", r.stderr)

    def test_clean_index_passes(self):
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("1 files ok", r.stdout)

    def test_z64_header_rejected_whatever_the_name(self):
        self.stage("game.bin", bytes.fromhex("80371240") + bytes(60))
        self.assert_rejected("game.bin", "N64 ROM header")

    def test_v64_header_rejected(self):
        self.stage("game.bin", bytes.fromhex("37804012") + bytes(60))
        self.assert_rejected("game.bin", "N64 ROM header")

    def test_wasm_rejected(self):
        self.stage("dk64.wasm", b"\0asm")
        self.assert_rejected("dk64.wasm", "build output")

    def test_audio_rejected(self):
        self.stage("song.ogg", b"OggS")
        self.assert_rejected("song.ogg", "audio or archive file")

    def test_zip_rejected(self):
        self.stage("pack.zip", b"PK\x03\x04")
        self.assert_rejected("pack.zip", "audio or archive file")

    def test_large_file_rejected(self):
        self.stage("big.dat", bytes(3 * 1024 * 1024))
        self.assert_rejected("big.dat", "larger than 2 MiB")


if __name__ == "__main__":
    unittest.main()
