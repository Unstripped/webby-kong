"""tools/packs.py: community audio pack upload, validation, listing, download and deletion (sign-in gate off)."""
import functools
import http.client
import io
import json
import sys
import tempfile
import threading
import unittest
import wave
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import packs  # noqa: E402
import saves  # noqa: E402
import serve  # noqa: E402


def tiny_wav():
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(bytes(800))
    return out.getvalue()


def make_zip(manifest=None, files=None, raw_names=()):
    """A pack zip: manifest.json (unless manifest is False), the given files, and empty entries named raw_names."""
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        if manifest is not False:
            zf.writestr("manifest.json", json.dumps(manifest if manifest is not None else {
                "format": 1, "name": "Test pack", "author": "me", "songs": {"0x10": {"file": "song.wav"}},
                "sfx": {"291": {"file": "beep.wav", "loop": False, "gain": 0.5}}}))
        for name, data in (files if files is not None else {"song.wav": tiny_wav(), "beep.wav": tiny_wav()}).items():
            zf.writestr(name, data)
        for name in raw_names:
            zf.writestr(zipfile.ZipInfo(name), b"x")
    return out.getvalue()


class PackServerTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        tmp = Path(self._tmp.name)
        self.db = tmp / "saves.sqlite3"
        saves.init(self.db)
        packs.init(self.db)

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

    def call(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("::1", self.port, timeout=5)
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        data = resp.read()
        conn.close()
        return resp, data

    def upload(self, data):
        resp, body = self.call("POST", packs.API, data, {"Content-Type": "application/zip"})
        return resp.status, json.loads(body)

    def assert_bad(self, data, message):
        status, body = self.upload(data)
        self.assertEqual((status, body["error"]), (400, "bad_pack"), body)
        self.assertIn(message, body["message"])

    def test_upload_list_download(self):
        data = make_zip()
        status, body = self.upload(data)
        self.assertEqual(status, 201, body)
        pack = body["pack"]
        self.assertEqual((pack["name"], pack["author"], pack["songs"], pack["sfx"], pack["size"]),
                         ("Test pack", "me", 1, 1, len(data)))
        self.assertEqual((pack["uploader"], pack["can_delete"]), (None, True))
        resp, listed = self.call("GET", packs.API)
        self.assertEqual(json.loads(listed)["packs"], [pack])
        resp, file = self.call("GET", f"{packs.API}/{pack['id']}/file")
        self.assertEqual((resp.status, resp.getheader("Content-Type")), (200, "application/zip"))
        self.assertEqual(file, data)

    def test_duplicate_is_409(self):
        data = make_zip()
        status, body = self.upload(data)
        self.assertEqual(status, 201)
        status, again = self.upload(data)
        self.assertEqual((status, again["error"]), (409, "duplicate"))
        self.assertIn(f"pack {body['pack']['id']}", again["message"])

    def test_bad_packs_are_refused(self):
        song = {"songs": {"16": {"file": "song.wav"}}}
        cases = [
            (b"not a zip at all", "not a zip file"),
            (make_zip(manifest=False), "no manifest.json"),
            (make_zip({"format": 2, "name": "x", **song}), "format must be 1"),
            (make_zip({"format": 1, "name": "x", "songs": {"16": {"file": "missing.ogg"}}}),
             'songs 0x10: file "missing.ogg" is not in the pack'),
            (make_zip({"format": 1, "name": "x", "songs": {"16": {"file": "song.wav", "loopStart": 2, "loopEnd": 2}}}),
             "songs 0x10: loopEnd is invalid"),
            (make_zip({"format": 1, "name": "x"}), "the pack replaces nothing (no songs or sfx)"),
            (make_zip(raw_names=["../x.ogg"]), 'entry "../x.ogg" has an unsafe name'),
        ]
        for data, message in cases:
            with self.subTest(message=message):
                self.assert_bad(data, message)
        self.assertEqual(json.loads(self.call("GET", packs.API)[1])["packs"], [])
        self.assertEqual(list(packs.pack_dir(self.db).iterdir()), [])

    def test_oversized_upload_is_413_before_the_body(self):
        conn = http.client.HTTPConnection("::1", self.port, timeout=5)
        conn.putrequest("POST", packs.API)
        conn.putheader("Content-Length", str(300 * 1024 * 1024))
        conn.endheaders()
        resp = conn.getresponse()
        body = json.loads(resp.read())
        conn.close()
        self.assertEqual((resp.status, body["error"]), (413, "too_large"))

    def test_delete_removes_the_row_and_the_file(self):
        status, body = self.upload(make_zip())
        pack_id = body["pack"]["id"]
        self.assertTrue((packs.pack_dir(self.db) / f"{pack_id}.zip").exists())
        self.assertEqual(self.call("DELETE", f"{packs.API}/{pack_id}")[0].status, 204)
        resp, body = self.call("GET", f"{packs.API}/{pack_id}/file")
        self.assertEqual((resp.status, json.loads(body)["error"]), (404, "not_found"))
        self.assertFalse((packs.pack_dir(self.db) / f"{pack_id}.zip").exists())


if __name__ == "__main__":
    unittest.main()
