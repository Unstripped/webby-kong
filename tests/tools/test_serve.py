"""tools/serve.py: the save games API (tools/saves.py) that web/saves.js relies on and the mounted ROM web/main.js
loads."""
import contextlib
import functools
import hashlib
import http.client
import io
import json
import os
import sqlite3
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import saves  # noqa: E402
import serve  # noqa: E402

SAVE_A = bytes(range(256)) * 8
SAVE_B = bytes(reversed(range(256))) * 8


class SaveServerTest(unittest.TestCase):
    """The sign-in gate is off here: every save belongs to the anonymous owner (user_id NULL)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        tmp = Path(self._tmp.name)
        self.db = tmp / "saves.sqlite3"
        saves.init(self.db)

        class QuietHandler(serve.Handler):
            db_path = self.db

            def log_message(self, *args):
                pass

        self.server = serve.Server(("::1", 0), functools.partial(QuietHandler, directory=str(tmp)))
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.save = self.create()["id"]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self._tmp.cleanup()

    def call(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("::1", self.port, timeout=5)
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        data = resp.read()
        conn.close()
        return resp, data

    def call_json(self, method, path, payload):
        return self.call(method, path, json.dumps(payload).encode(), {"Content-Type": "application/json"})

    def create(self, name=None):
        resp, data = self.call_json("POST", saves.API, {} if name is None else {"name": name})
        self.assertEqual(resp.status, 201, data)
        return json.loads(data)["save"]

    def get_save(self, save_id):
        resp, data = self.call("GET", f"{saves.API}/{save_id}")
        self.assertEqual(resp.status, 200, data)
        return json.loads(data)["save"]

    def request(self, method, body=None, mtime=None, save=None, summary=None):
        headers = {} if mtime is None else {"X-Save-Mtime": str(mtime)}
        if summary is not None:
            headers["X-Save-Summary"] = summary if isinstance(summary, str) else json.dumps(summary)
        return self.call(method, f"{saves.API}/{save or self.save}/eeprom", body, headers)

    def versions(self, save=None):
        with contextlib.closing(sqlite3.connect(self.db)) as conn:
            return conn.execute("select count(*) from eeprom where save_id = ?", (save or self.save,)).fetchone()[0]

    @staticmethod
    def summary(percent):
        return dict({key: 0 for key in saves.SUMMARY_KEYS}, percent=percent, playSeconds=754)

    def test_no_save_is_404(self):
        resp, data = self.request("GET")
        self.assertEqual((resp.status, json.loads(data)["error"]), (404, "no_save_data"))

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
        self.assertEqual(self.versions(), 1)

    def test_two_saves_keep_separate_histories(self):
        other = self.create("Other")["id"]
        self.request("PUT", SAVE_A, 1000)
        self.request("PUT", SAVE_B, 1000, save=other)
        self.request("PUT", SAVE_A, 2000, save=other)
        self.assertEqual(self.request("GET")[1], SAVE_A)
        self.assertEqual((self.versions(), self.versions(other)), (1, 2))
        self.request("PUT", SAVE_B, 3000)
        self.assertEqual(self.request("GET", save=other)[1], SAVE_A)

    def test_summary_is_stored_and_listed(self):
        resp, _ = self.request("PUT", SAVE_A, 1000, summary=self.summary(12))
        self.assertEqual(resp.status, 204)
        resp, data = self.call("GET", saves.API)
        listed = json.loads(data)
        self.assertFalse(listed["dashboard"])
        self.assertEqual(listed["saves"][0]["summary"]["percent"], 12)
        self.assertIsNotNone(listed["saves"][0]["played_at"])

    def test_a_malformed_summary_is_refused_and_not_stored(self):
        for bad in ("{", json.dumps({"percent": 12}), json.dumps(dict(self.summary(12), percent=True)),
                    json.dumps(dict(self.summary(12), extra=1)), json.dumps(dict(self.summary(12), keys=-1))):
            resp, data = self.request("PUT", SAVE_A, 1000, summary=bad)
            self.assertEqual((resp.status, json.loads(data)["error"]), (400, "summary_invalid"), bad)
        self.assertEqual(self.versions(), 0)
        self.assertIsNone(self.get_save(self.save)["summary"])

    def test_resume_point_round_trips_and_clears(self):
        point = {"slot": 0, "kong": 1, "map": 7, "exit": 0, "hasPos": 1, "x": 1642.5, "y": 330, "z": 1519,
                 "yaw": 2048, "ok": True}
        resp, _ = self.call_json("PUT", f"{saves.API}/{self.save}/resume", {"resume": point})
        self.assertEqual(resp.status, 204)
        resume = self.get_save(self.save)["resume"]
        self.assertEqual({k: resume[k] for k in saves.RESUME_KEYS}, {k: point[k] for k in saves.RESUME_KEYS})
        self.assertNotIn("ok", resume)
        self.assertRegex(resume["time"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$")
        resp, data = self.call_json("PUT", f"{saves.API}/{self.save}/resume", {"resume": dict(point, x="far")})
        self.assertEqual((resp.status, json.loads(data)["error"]), (400, "resume_invalid"))
        resp, _ = self.call_json("PUT", f"{saves.API}/{self.save}/resume", {"resume": None})
        self.assertEqual(resp.status, 204)
        self.assertIsNone(self.get_save(self.save)["resume"])

    def test_create_names_rename_and_delete(self):
        self.assertEqual(self.get_save(self.save)["name"], "Save 1")
        self.assertEqual(self.create()["name"], "Save 2")
        resp, data = self.call_json("POST", saves.API, {"name": "x" * 41})
        self.assertEqual((resp.status, json.loads(data)["error"]), (400, "name_invalid"))
        resp, data = self.call_json("PATCH", f"{saves.API}/{self.save}", {"name": "  Speedrun  "})
        self.assertEqual((resp.status, json.loads(data)["save"]["name"]), (200, "Speedrun"))
        resp, data = self.call_json("PATCH", f"{saves.API}/{self.save}", {"name": " "})
        self.assertEqual((resp.status, json.loads(data)["error"]), (400, "name_invalid"))
        self.request("PUT", SAVE_A, 1000)
        resp, _ = self.call("DELETE", f"{saves.API}/{self.save}")
        self.assertEqual(resp.status, 204)
        self.assertEqual(self.versions(), 0)
        self.assertEqual(self.call("GET", f"{saves.API}/{self.save}")[0].status, 404)
        # ids are never reused, not even the highest: the browser keys its EEPROM file and quicksaves by the id
        newest = self.create()["id"]
        self.assertEqual(self.call("DELETE", f"{saves.API}/{newest}")[0].status, 204)
        self.assertGreater(self.create()["id"], newest)

    def test_json_endpoints_take_json_only(self):
        resp, data = self.call("POST", saves.API, b'{"name": "x"}', {"Content-Type": "text/plain"})
        self.assertEqual((resp.status, json.loads(data)["error"]), (400, "request_invalid"))

    def test_unknown_ids_and_paths_are_404(self):
        for method, path in (("GET", f"{saves.API}/999"), ("GET", f"{saves.API}/999/eeprom"),
                             ("DELETE", f"{saves.API}/999"), ("GET", f"{saves.API}/{self.save}/nope"),
                             ("POST", f"{saves.API}/{self.save}")):
            resp, data = self.call(method, path)
            self.assertEqual((resp.status, json.loads(data)["error"]), (404, "not_found"), (method, path))
        resp, _ = self.request("PUT", SAVE_A, 1000, save=999)
        self.assertEqual(resp.status, 404)

    def test_list_is_most_recently_played_first(self):
        second = self.create("Second")["id"]
        third = self.create("Third")["id"]
        self.request("PUT", SAVE_A, 1000, save=second)
        self.request("PUT", SAVE_A, 1000)
        ids = [s["id"] for s in json.loads(self.call("GET", saves.API)[1])["saves"]]
        self.assertEqual(ids, [self.save, second, third])


class LegacyDatabaseTest(unittest.TestCase):
    """A database from before save games had one shared history: it becomes the save "Save 1"."""

    def test_shared_history_becomes_one_save(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "saves.sqlite3"
            with contextlib.closing(sqlite3.connect(db)) as conn:
                conn.executescript("""
                    CREATE TABLE eeprom (id INTEGER PRIMARY KEY, mtime_ms INTEGER NOT NULL,
                      received_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
                      sha1 TEXT NOT NULL, bytes BLOB NOT NULL);
                    CREATE INDEX eeprom_mtime ON eeprom(mtime_ms);""")
                conn.execute("INSERT INTO eeprom (mtime_ms, received_at, sha1, bytes) VALUES "
                             "(1, '2025-01-01T00:00:00.000Z', 'a', x'00'), (2, '2025-02-01T00:00:00.000Z', 'b', x'01')")
                conn.commit()
            for _ in range(2):
                saves.init(db)
                with contextlib.closing(sqlite3.connect(db)) as conn:
                    rows = conn.execute("SELECT id, user_id, name, played_at FROM saves").fetchall()
                    self.assertEqual(rows, [(1, None, "Save 1", "2025-02-01T00:00:00.000Z")])
                    self.assertEqual(conn.execute("SELECT save_id FROM eeprom ORDER BY id").fetchall(), [(1,), (1,)])


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


class WebFilesTest(unittest.TestCase):
    """The server refuses to start on a directory that is not a build of the page (index.html), and with the gate
    on also on one the build never copied web/auth/ and web/dashboard/ into: otherwise every URL is a bare 404."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def pages(self, *names):
        for name in names:
            (self.dir / name).parent.mkdir(parents=True, exist_ok=True)
            (self.dir / name).write_text("")

    def test_an_empty_directory_lacks_every_page_in_order(self):
        self.assertEqual(serve.missing_web_files(self.dir, False), ["index.html"])
        self.assertEqual(serve.missing_web_files(self.dir, True),
                         ["index.html", "auth/index.html", "dashboard/index.html"])

    def test_a_present_page_is_not_missing(self):
        self.pages("index.html", "auth/index.html")
        self.assertEqual(serve.missing_web_files(self.dir, True), ["dashboard/index.html"])
        self.pages("dashboard/index.html")
        self.assertEqual(serve.missing_web_files(self.dir, True), [])

    def test_gate_off_refuses_a_missing_directory(self):
        missing = self.dir / "web"
        with self.assertRaises(SystemExit) as caught:
            serve.require_web_files(missing, False)
        message = str(caught.exception)
        self.assertIn(str(missing.resolve()), message)
        self.assertIn("does not exist", message)
        self.assertIn("cmake --build build/web", message)
        self.assertIn("WEBBY_KONG_DIR", message)

    def test_gate_off_passes_with_index_html(self):
        self.pages("index.html")
        serve.require_web_files(self.dir, False)

    def test_gate_on_requires_the_sign_in_pages(self):
        self.pages("index.html")
        with self.assertRaises(SystemExit) as caught:
            serve.require_web_files(self.dir, True)
        message = str(caught.exception)
        self.assertIn("auth/index.html", message)
        self.assertIn("dashboard/index.html", message)
        self.assertNotIn("has no index.html", message)
        self.pages("auth/index.html", "dashboard/index.html")
        serve.require_web_files(self.dir, True)

    def test_main_refuses_before_touching_the_database(self):
        db = self.dir / "saves.sqlite3"
        for environ in ({"WEBBY_KONG_AUTH": "", "WEBBY_KONG_DB": str(db)},
                        {"WEBBY_KONG_AUTH": "1", "WEBBY_KONG_DB": str(db)}):
            with mock.patch.dict(os.environ, environ), self.assertRaises(SystemExit) as caught:
                serve.main(["--dir", str(self.dir)])
            self.assertIn("index.html", str(caught.exception))
            self.assertFalse(db.exists())
        self.assertFalse(Path(f"{db}-wal").exists())


if __name__ == "__main__":
    unittest.main()
