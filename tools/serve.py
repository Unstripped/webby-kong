#!/usr/bin/env python3
"""Serve build/web and keep every version of the game's EEPROM save in SQLite.

Static files are served like `python3 -m http.server -d build/web 8080`. web/saves.js adds:

  GET /api/eeprom   latest save (highest client mtime): 2048 bytes, X-Save-Mtime / X-Save-Sha1 headers; 404 if none
  PUT /api/eeprom   2048-byte body plus an X-Save-Mtime header (ms since epoch); stored unless it repeats the
                    latest version or an identical (sha1, mtime) row

Recovery with the sqlite3 CLI (DB defaults to ~/.local/share/webby-kong/saves.sqlite3):

  list versions        sqlite3 DB "select id, mtime_ms, received_at, sha1 from eeprom order by id"
  export version N     sqlite3 DB "select writefile('x.eep', bytes) from eeprom where id=N"
  make N current       sqlite3 DB "insert into eeprom (mtime_ms, sha1, bytes) select
                         cast(strftime('%s','now') as integer)*1000, sha1, bytes from eeprom where id=N"
                       (the newest mtime wins at the next page load)
"""
import argparse
import contextlib
import functools
import hashlib
import http.server
import socket
import sqlite3
import urllib.parse
from pathlib import Path

API_PATH = "/api/eeprom"
EEPROM_SIZE = 2048
SCHEMA = """
CREATE TABLE IF NOT EXISTS eeprom (
  id INTEGER PRIMARY KEY,
  mtime_ms INTEGER NOT NULL,
  received_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  sha1 TEXT NOT NULL,
  bytes BLOB NOT NULL);
CREATE INDEX IF NOT EXISTS eeprom_mtime ON eeprom(mtime_ms);
"""
LATEST = "SELECT mtime_ms, sha1, bytes FROM eeprom ORDER BY mtime_ms DESC, id DESC LIMIT 1"


class Server(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6

    def server_bind(self):
        # dual stack, like `python3 -m http.server`: localhost over IPv4 and IPv6 is the same origin as before
        with contextlib.suppress(Exception):
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        super().server_bind()


class Handler(http.server.SimpleHTTPRequestHandler):
    db_path: Path

    def db(self):
        return contextlib.closing(sqlite3.connect(self.db_path, timeout=5, isolation_level=None))

    def is_api(self):
        return urllib.parse.urlsplit(self.path).path == API_PATH

    def do_GET(self):
        if not self.is_api():
            return super().do_GET()
        with self.db() as conn:
            row = conn.execute(LATEST).fetchone()
        if row is None:
            return self.send_error(404, "no save stored")
        mtime, sha1, data = row
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Save-Mtime", str(mtime))
        self.send_header("X-Save-Sha1", sha1)
        self.end_headers()
        self.wfile.write(data)

    def do_PUT(self):
        if not self.is_api():
            return self.send_error(404)
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return self.send_error(400, "Content-Length required")
        if length != EEPROM_SIZE:
            return self.send_error(400, f"body must be {EEPROM_SIZE} bytes")
        body = self.rfile.read(length)
        mtime_header = self.headers.get("X-Save-Mtime", "")
        if len(body) != EEPROM_SIZE or not mtime_header.isascii() or not mtime_header.isdigit():
            return self.send_error(400, "X-Save-Mtime must be a non-negative integer")
        mtime = int(mtime_header)
        sha1 = hashlib.sha1(body).hexdigest()
        with self.db() as conn:
            conn.execute("BEGIN IMMEDIATE")
            latest = conn.execute(LATEST).fetchone()
            seen = conn.execute("SELECT 1 FROM eeprom WHERE sha1 = ? AND mtime_ms = ?", (sha1, mtime)).fetchone()
            if (latest is None or latest[1] != sha1) and seen is None:
                conn.execute("INSERT INTO eeprom (mtime_ms, sha1, bytes) VALUES (?, ?, ?)", (mtime, sha1, body))
            conn.execute("COMMIT")
        self.send_response(204)
        self.end_headers()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default="build/web", help="directory to serve (default build/web)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="::", help="address to listen on (default :: = all interfaces)")
    ap.add_argument("--db", type=Path, default=Path.home() / ".local/share/webby-kong/saves.sqlite3")
    args = ap.parse_args()

    args.db.parent.mkdir(parents=True, exist_ok=True)
    with contextlib.closing(sqlite3.connect(args.db)) as conn:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.executescript(SCHEMA)

    Handler.db_path = args.db
    handler = functools.partial(Handler, directory=args.dir)
    with Server((args.bind, args.port), handler) as httpd:
        print(f"serving {args.dir} on [{args.bind}]:{args.port}, saves in {args.db}", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
