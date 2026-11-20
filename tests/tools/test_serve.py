"""tools/serve.py: the EEPROM save API that web/saves.js relies on."""
import contextlib
import functools
import hashlib
import http.client
import sqlite3
import sys
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import serve  # noqa: E402

SAVE_A = bytes(range(256)) * 8
SAVE_B = bytes(reversed(range(256))) * 8


class SaveServerTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        tmp = Path(self._tmp.name)
        self.db = tmp / "saves.sqlite3"
        with contextlib.closing(sqlite3.connect(self.db)) as conn:
            conn.executescript(serve.SCHEMA)

        class QuietHandler(serve.Handler):
            db_path = self.db

            def log_message(self, *args):
                pass

        self.server = serve.Server(("::1", 0), functools.partial(QuietHandler, directory=str(tmp)))
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self._tmp.cleanup()

    def request(self, method, body=None, mtime=None):
        conn = http.client.HTTPConnection("::1", self.port, timeout=5)
        headers = {} if mtime is None else {"X-Save-Mtime": str(mtime)}
        conn.request(method, serve.API_PATH, body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        conn.close()
        return resp, data

    def test_no_save_is_404(self):
        resp, _ = self.request("GET")
        self.assertEqual(resp.status, 404)

    def test_put_then_get_round_trips(self):
        resp, _ = self.request("PUT", SAVE_A, 1000)
        self.assertEqual(resp.status, 204)
        resp, data = self.request("GET")
        self.assertEqual(resp.status, 200)
        self.assertEqual(data, SAVE_A)
        self.assertEqual(resp.getheader("X-Save-Mtime"), "1000")
        self.assertEqual(resp.getheader("X-Save-Sha1"), hashlib.sha1(SAVE_A).hexdigest())

    def test_newest_mtime_wins(self):
        self.request("PUT", SAVE_A, 2000)
        self.request("PUT", SAVE_B, 1000)
        _, data = self.request("GET")
        self.assertEqual(data, SAVE_A)

    def test_wrong_size_rejected(self):
        resp, _ = self.request("PUT", SAVE_A[:-1], 1000)
        self.assertEqual(resp.status, 400)
        resp, _ = self.request("GET")
        self.assertEqual(resp.status, 404)

    def test_repeat_of_latest_not_stored(self):
        self.request("PUT", SAVE_A, 1000)
        self.request("PUT", SAVE_A, 3000)
        with contextlib.closing(sqlite3.connect(self.db)) as conn:
            self.assertEqual(conn.execute("select count(*) from eeprom").fetchone()[0], 1)


if __name__ == "__main__":
    unittest.main()
