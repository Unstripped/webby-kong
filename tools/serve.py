#!/usr/bin/env python3
"""Serve build/web, keep every version of the game's EEPROM save in SQLite and serve a mounted ROM.

Static files are served like `python3 -m http.server -d build/web 8080`. web/saves.js and web/main.js add:

  GET /api/eeprom   latest save (highest client mtime): 2048 bytes, X-Save-Mtime / X-Save-Sha1 headers; 404 if none
  PUT /api/eeprom   2048-byte body plus an X-Save-Mtime header (ms since epoch); stored unless it repeats the
                    latest version or an identical (sha1, mtime) row
  GET /api/rom      the --rom file (or the first *.z64/*.n64/*.v64 in the --rom directory), looked up per request;
                    404 if none. web/main.js starts it without the file picker (the Docker image mounts /rom)

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
ROM_PATH = "/api/rom"
ROM_SUFFIXES = {".z64", ".n64", ".v64"}
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


def find_rom(path):
    """--rom value -> the ROM file to serve, or None."""
    if path is None:
        return None
    if path.is_file():
        return path
    if path.is_dir():
        roms = sorted(p for p in path.iterdir() if p.is_file() and p.suffix.lower() in ROM_SUFFIXES)
        return roms[0] if roms else None
    return None


class Server(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6

    def server_bind(self):
        # dual stack, like `python3 -m http.server`: localhost over IPv4 and IPv6 is the same origin as before
        if self.address_family == socket.AF_INET6:
            with contextlib.suppress(Exception):
                self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        super().server_bind()


class Handler(http.server.SimpleHTTPRequestHandler):
    db_path: Path
    rom_path: Path | None = None

    def db(self):
        return contextlib.closing(sqlite3.connect(self.db_path, timeout=5, isolation_level=None))

    def is_api(self):
        return urllib.parse.urlsplit(self.path).path == API_PATH

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == ROM_PATH:
            return self.send_rom()
        if path != API_PATH:
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

    def send_rom(self):
        rom = find_rom(self.rom_path)
        if rom is None:
            return self.send_error(404, "no ROM mounted")
        with open(rom, "rb") as f:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(rom.stat().st_size))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.copyfile(f, self.wfile)

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
    ap.add_argument("--rom", type=Path,
                    help="ROM file, or a directory holding one *.z64/*.n64/*.v64; served at /api/rom")
    args = ap.parse_args()

    args.db.parent.mkdir(parents=True, exist_ok=True)
    with contextlib.closing(sqlite3.connect(args.db)) as conn:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.executescript(SCHEMA)

    Handler.db_path = args.db
    Handler.rom_path = args.rom
    if ":" not in args.bind:
        Server.address_family = socket.AF_INET
    handler = functools.partial(Handler, directory=args.dir)
    with Server((args.bind, args.port), handler) as httpd:
        print(f"serving {args.dir} on [{args.bind}]:{args.port}, saves in {args.db}", flush=True)
        if args.rom is not None:
            rom = find_rom(args.rom)
            print(f"rom: {rom}" if rom else f"rom: none in {args.rom}", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
