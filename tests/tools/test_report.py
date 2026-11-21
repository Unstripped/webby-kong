"""tools/report.py: decoding the in-game bug report blob (web/report.js) from issue text."""
import base64
import json
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from report import extract, resume_arg  # noqa: E402


def encode(obj):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    raw = c.compress(json.dumps(obj).encode()) + c.flush()
    return "DK64R1." + base64.urlsafe_b64encode(raw).decode().rstrip("=")


class ExtractTest(unittest.TestCase):
    def test_round_trip_from_issue_markdown(self):
        obj = {"v": 1, "save": "00ff" * 4, "resume": {"map": 176}, "mark": {"fx": 0.4, "fy": 0.55, "probe": "#3 dl=x"}}
        issue = (
            "### Game report\n\n| | |\n| --- | --- |\n| **Version** | source build |\n\n"
            "<details><summary>Save, location &amp; probe</summary>\n\n```text\n" + encode(obj) + "\n```\n\n</details>\n"
        )
        self.assertEqual(extract(issue), obj)

    def test_no_blob(self):
        with self.assertRaises(ValueError):
            extract("just a bug, no report")

    def test_unknown_version(self):
        with self.assertRaises(ValueError):
            extract(encode({"v": 2}))


class ResumeArgTest(unittest.TestCase):
    def test_matches_headless_scanf_order(self):
        r = {"slot": 0, "kong": 0, "map": 176, "exit": 1, "hasPos": 1, "x": 941.0, "y": 60.0, "z": 736.0, "yaw": 3752}
        self.assertEqual(resume_arg(r), "0,0,176,1,1,941.0,60.0,736.0,3752")


if __name__ == "__main__":
    unittest.main()
