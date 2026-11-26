"""port/src/gfx/gfx_rt64hash.c reproduces RT64's TMEMHasher, so RT64 texture packs match this port's textures.

The expected hashes were produced by RT64's own TMEMHasher (commit d9817c68ffd7328cd0af383af124971489366e18) with
xxHash 0.8.3 over the same TMEM pattern and tiles as tests/tools/rt64hash_test.c."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [ROOT / "tests" / "tools" / "rt64hash_test.c", ROOT / "port" / "src" / "gfx" / "gfx_rt64hash.c"]
INCLUDE = ROOT / "port" / "src"

EXPECTED = """\
rgba16_32x32_v5 32 32 0 9fed30ea28daa3a7
rgba16_32x32_v1 32 32 0 9fed30ea28daa3a7
ci4_16x16_rgba_v5 16 16 0 e16f4d7b981f5179
ci4_16x16_rgba_v4 16 16 0 e16f4d7b981f5179
ci8_32x16_ia_clamp_v5 32 16 0 e31e49e56d0c120c
rgba32_16x16_v5 16 16 0 8d6247f67ef24a7c
ia8_32x8_rows_v5 32 8 0 083f4c017c6e6107
ia8_32x8_rows_v3 32 8 0 083f4c017c6e6107
i4_negspan_clamp_v5 1 8 0 f27cb2cd4dce2dcd
rgba16_wrap_v5 64 32 0 4fd5575ce1fe78d0
ci4_2x1_partial_v5 2 1 0 2a7316bfe542e98a
ci4_2x1_partial_v4 2 1 0 58807653c591acd4
ia8_12x4_v5 12 4 0 2a34fd5913831002
ia8_12x4_v3 12 4 0 58a3fbf6eeb62073
ia8_12x4_v1 12 4 0 0d304f4c8e98b0fb
rgba16_raw_v5 256 64 1 25b0cd34384922a6
"""


def build_and_run(tmp):
    """Compiles the test program with emcc (run under node) or a native C compiler; None when there is neither."""
    common = ["-O1", "-I", str(INCLUDE), *map(str, SOURCES)]
    emcc = shutil.which("emcc", path=os.environ.get("PATH"))
    node = shutil.which("node")
    if emcc and node:
        out = Path(tmp) / "t.cjs"
        subprocess.run([emcc, *common, "-o", str(out)], cwd=ROOT, check=True, capture_output=True)
        return subprocess.run([node, str(out)], check=True, capture_output=True, text=True).stdout
    for cc in ("cc", "gcc", "clang"):
        path = shutil.which(cc)
        if path:
            out = Path(tmp) / "t.exe"
            subprocess.run([path, *common, "-o", str(out)], cwd=ROOT, check=True, capture_output=True)
            return subprocess.run([str(out)], check=True, capture_output=True, text=True).stdout
    return None


class Rt64HashTest(unittest.TestCase):
    def test_matches_rt64(self):
        with tempfile.TemporaryDirectory() as tmp:
            try:
                got = build_and_run(tmp)
            except subprocess.CalledProcessError as e:
                self.fail(f"{e.cmd[0]} failed:\n{e.stdout}\n{e.stderr}")
        if got is None:
            self.skipTest("no C compiler")
        self.assertEqual(got.replace("\r\n", "\n").splitlines(), EXPECTED.splitlines())


if __name__ == "__main__":
    unittest.main()
