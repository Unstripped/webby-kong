"""tools/serve.py: the EEPROM save API that web/saves.js relies on and the mounted ROM web/main.js loads."""
import contextlib
import functools
import hashlib
import http.client
import io
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


class RomServerTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.rom_dir = self.tmp / "rom"
        self.rom_dir.mkdir()
        self.server = None

    def tearDown(self):
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
        self._tmp.cleanup()

    def get_rom(self, rom_path):
        class QuietHandler(serve.Handler):
            db_path = self.tmp / "saves.sqlite3"

            def log_message(self, *args):
                pass

        QuietHandler.rom_path = rom_path
        self.server = serve.Server(("::1", 0), functools.partial(QuietHandler, directory=str(self.tmp)))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        conn = http.client.HTTPConnection("::1", self.server.server_address[1], timeout=5)
        conn.request("GET", serve.ROM_PATH)
        resp = conn.getresponse()
        data = resp.read()
        conn.close()
        return resp, data

    def test_directory_serves_first_rom_by_name(self):
        (self.rom_dir / "b.z64").write_bytes(b"B" * 64)
        (self.rom_dir / "a.v64").write_bytes(b"A" * 32)
        (self.rom_dir / "0-notes.txt").write_bytes(b"not a rom")
        resp, data = self.get_rom(self.rom_dir)
        self.assertEqual(resp.status, 200)
        self.assertEqual(data, b"A" * 32)
        self.assertEqual(resp.getheader("Cache-Control"), "no-store")

    def test_empty_directory_is_404(self):
        resp, _ = self.get_rom(self.rom_dir)
        self.assertEqual(resp.status, 404)

    def test_no_rom_option_is_404(self):
        resp, _ = self.get_rom(None)
        self.assertEqual(resp.status, 404)


class ConfigTest(unittest.TestCase):
    """serve.resolve(): a flag overrides WEBBY_KONG_*, which overrides the default; describe() hides secrets."""

    def test_defaults(self):
        settings = serve.resolve([], {})
        self.assertEqual(settings.dir, Path("build/web"))
        self.assertEqual(settings.port, 8080)
        self.assertEqual(settings.bind, "::")
        self.assertEqual(settings.db, Path.home() / ".local/share/webby-kong/saves.sqlite3")
        self.assertIsNone(settings.rom)
        self.assertFalse(settings.auth.enabled)

    def test_environment_is_used(self):
        settings = serve.resolve([], {"WEBBY_KONG_DIR": "/srv/web", "WEBBY_KONG_PORT": "9000",
                                      "WEBBY_KONG_BIND": "0.0.0.0", "WEBBY_KONG_DB": "/data/saves.sqlite3",
                                      "WEBBY_KONG_ROM": "/rom"})
        self.assertEqual((settings.dir, settings.port, settings.bind, settings.db, settings.rom),
                         (Path("/srv/web"), 9000, "0.0.0.0", Path("/data/saves.sqlite3"), Path("/rom")))

    def test_flag_overrides_environment(self):
        settings = serve.resolve(["--dir", "build/web", "--port", "8081", "--bind", "127.0.0.1",
                                  "--db", "local.sqlite3", "--rom", "dk64.z64"],
                                 {"WEBBY_KONG_DIR": "/srv/web", "WEBBY_KONG_PORT": "9000",
                                  "WEBBY_KONG_BIND": "0.0.0.0", "WEBBY_KONG_DB": "/data/saves.sqlite3",
                                  "WEBBY_KONG_ROM": "/rom"})
        self.assertEqual((settings.dir, settings.port, settings.bind, settings.db, settings.rom),
                         (Path("build/web"), 8081, "127.0.0.1", Path("local.sqlite3"), Path("dk64.z64")))

    def test_empty_variables_are_the_defaults(self):
        settings = serve.resolve([], {"WEBBY_KONG_DIR": "", "WEBBY_KONG_PORT": "", "WEBBY_KONG_BIND": " ",
                                      "WEBBY_KONG_DB": "", "WEBBY_KONG_ROM": ""})
        self.assertEqual((settings.dir, settings.port, settings.bind),
                         (Path("build/web"), 8080, "::"))
        self.assertIsNone(settings.rom)

    def test_home_in_a_variable_is_expanded(self):
        self.assertEqual(serve.resolve([], {"WEBBY_KONG_DB": "~/saves.sqlite3"}).db,
                         Path.home() / "saves.sqlite3")

    def test_a_bad_port_names_the_variable(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
            serve.resolve([], {"WEBBY_KONG_PORT": "http"})
        self.assertIn("WEBBY_KONG_PORT", stderr.getvalue())

    def test_sign_in_settings_come_from_the_same_environment(self):
        settings = serve.resolve([], {"WEBBY_KONG_AUTH": "1",
                                      "WEBBY_KONG_EXTERNAL_URL": "https://kong.example.com"})
        self.assertTrue(settings.auth.enabled)
        self.assertEqual(settings.auth.origin, "https://kong.example.com")
        self.assertEqual(settings.auth.rp_id, "kong.example.com")

    def test_describe_prints_every_setting_but_the_smtp_password(self):
        settings = serve.resolve([], {"WEBBY_KONG_AUTH": "1", "WEBBY_KONG_SMTP_HOST": "smtp.example.com",
                                      "WEBBY_KONG_SMTP_USERNAME": "kong", "WEBBY_KONG_SMTP_PASSWORD": "hunter2"})
        text = "\n".join(serve.describe(settings))
        self.assertIn("dir=build/web", text)
        self.assertIn("port=8080", text)
        self.assertIn("auth=on", text)
        self.assertIn("password=set", text)
        self.assertNotIn("hunter2", text)

    def test_describe_with_the_gate_off(self):
        lines = serve.describe(serve.resolve([], {}))
        self.assertIn("rom=(none)", lines)
        self.assertIn("auth=off", lines)
        self.assertEqual([line for line in lines if line.startswith("auth.")], [])


if __name__ == "__main__":
    unittest.main()
