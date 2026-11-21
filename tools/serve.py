#!/usr/bin/env python3
"""Serve build/web, keep every version of the game's EEPROM save in SQLite and serve a mounted ROM.

Static files are served like `python3 -m http.server -d build/web 8080`. web/saves.js and web/main.js add:

  GET /api/eeprom   latest save (highest client mtime): 2048 bytes, X-Save-Mtime / X-Save-Sha1 headers; 404 if none
  PUT /api/eeprom   2048-byte body plus an X-Save-Mtime header (ms since epoch); stored unless it repeats the
                    latest version or an identical (sha1, mtime) row
  GET /api/rom      the --rom file (or the first *.z64/*.n64/*.v64 in the --rom directory), looked up per request;
                    404 if none. web/main.js starts it without the file picker (the Docker image mounts /rom)

WEBBY_KONG_AUTH=1 puts an optional passkey sign-in gate in front of all of that (tools/auth/): the splash page lives
at /auth/, the dashboard at /, the game at /play, and everything else needs a session cookie. Without the variable
every URL behaves exactly as described above. web/ reaches the served directory through the build, so a missing
directory or index.html (and, with the gate on, auth/index.html or dashboard/index.html) stops the server at startup
instead of serving 404s.

Every setting comes from a WEBBY_KONG_* environment variable, and a command-line flag overrides it:

  --dir   WEBBY_KONG_DIR    directory to serve (default build/web)
  --port  WEBBY_KONG_PORT   port to listen on (default 8080)
  --bind  WEBBY_KONG_BIND   address to listen on (default :: = all interfaces)
  --db    WEBBY_KONG_DB     saves database (default ~/.local/share/webby-kong/saves.sqlite3)
  --rom   WEBBY_KONG_ROM    ROM file or directory served at /api/rom (default none)

The Docker image sets the same variables (WEBBY_KONG_DIR=/srv/web, WEBBY_KONG_BIND=0.0.0.0,
WEBBY_KONG_DB=/data/saves.sqlite3, WEBBY_KONG_ROM=/rom), so `docker run --env-file .env` and `mise run serve` take
one file of settings. The effective configuration (including the sign-in settings, with the SMTP password only as
set/unset) is printed at startup.

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
import os
import socket
import sqlite3
import urllib.parse
from dataclasses import dataclass
from pathlib import Path

import auth
from auth import config as auth_config

API_PATH = "/api/eeprom"
ROM_PATH = "/api/rom"
ROM_SUFFIXES = {".z64", ".n64", ".v64"}
EEPROM_SIZE = 2048
ENV_VARS = {"dir": "WEBBY_KONG_DIR", "port": "WEBBY_KONG_PORT", "bind": "WEBBY_KONG_BIND",
            "db": "WEBBY_KONG_DB", "rom": "WEBBY_KONG_ROM"}
DEFAULT_DIR = "build/web"
DEFAULT_PORT = 8080
DEFAULT_BIND = "::"
DEFAULT_DB = "~/.local/share/webby-kong/saves.sqlite3"
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
# the pages the gate redirects to (web/auth/, web/dashboard/): missing means the served copy predates them
SIGN_IN_PAGES = ("auth/index.html", "dashboard/index.html")


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


def missing_web_files(directory, auth_enabled):
    """The pages the served directory lacks: index.html, plus SIGN_IN_PAGES with the gate on, in that order."""
    required = ("index.html",) + (SIGN_IN_PAGES if auth_enabled else ())
    return [name for name in required if not (Path(directory) / name).is_file()]


def require_web_files(directory, auth_enabled):
    """Stop at startup when the served directory is not a (current) build of the page.

    web/ is copied into build/web by the build (docs/building.md), so an unbuilt worktree or a build made before a
    page was added would otherwise answer every request, or /auth/, with a bare 404.
    """
    path = Path(directory).resolve()
    if not path.is_dir():
        problem = "does not exist"
    else:
        missing = missing_web_files(path, auth_enabled)
        if not missing:
            return
        problem = f"has no {', '.join(missing)}"
    raise SystemExit(f"serve: {path} {problem}: build the page (docs/building.md: `tools/prepare.py all`, "
                     "`emcmake cmake -S . -B build/web -G Ninja ...`, `cmake --build build/web`) "
                     "or point WEBBY_KONG_DIR/--dir at a built directory")


@dataclass(frozen=True)
class Settings:
    """One server process's configuration: flags over WEBBY_KONG_* variables over the defaults, plus sign-in."""
    dir: Path
    port: int
    bind: str
    db: Path
    rom: Path | None
    auth: auth_config.Config


def parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", metavar="PATH", help=f"directory to serve (env {ENV_VARS['dir']}, default {DEFAULT_DIR})")
    ap.add_argument("--port", type=int, metavar="PORT",
                    help=f"port to listen on (env {ENV_VARS['port']}, default {DEFAULT_PORT})")
    ap.add_argument("--bind", metavar="ADDRESS",
                    help=f"address to listen on (env {ENV_VARS['bind']}, default {DEFAULT_BIND} = all interfaces)")
    ap.add_argument("--db", metavar="PATH", help=f"saves database (env {ENV_VARS['db']}, default {DEFAULT_DB})")
    ap.add_argument("--rom", metavar="PATH",
                    help="ROM file, or a directory holding one *.z64/*.n64/*.v64, served at /api/rom "
                         f"(env {ENV_VARS['rom']}, default none)")
    return ap


def resolve(argv=None, environ=None):
    """Flags over WEBBY_KONG_* over the defaults; argv=None and environ=None mean sys.argv and os.environ."""
    env = os.environ if environ is None else environ
    ap = parser()
    args = ap.parse_args(argv)

    def value(name):
        chosen = getattr(args, name)
        if chosen is None:
            chosen = (env.get(ENV_VARS[name]) or "").strip() or None
        return chosen

    port = value("port")
    if port is None:
        port = DEFAULT_PORT
    else:
        try:
            port = int(port)
        except ValueError:
            ap.error(f"{ENV_VARS['port']}: {port!r} is not a port")

    rom = value("rom")
    return Settings(
        dir=Path(value("dir") or DEFAULT_DIR).expanduser(),
        port=port,
        bind=value("bind") or DEFAULT_BIND,
        db=Path(value("db") or DEFAULT_DB).expanduser(),
        rom=Path(rom).expanduser() if rom is not None else None,
        auth=auth_config.from_env(env),
    )


def describe(settings):
    """The effective configuration as printable lines; the SMTP password is only ever reported as set or unset."""
    rom = find_rom(settings.rom) if settings.rom is not None else None
    rom_line = "rom=(none)" if settings.rom is None else f"rom={settings.rom} ({rom or 'no ROM file'})"
    lines = [
        f"dir={settings.dir}",
        f"bind={settings.bind}",
        f"port={settings.port}",
        f"db={settings.db}",
        rom_line,
    ]
    auth = settings.auth
    lines.append(f"auth={'on' if auth.enabled else 'off'}")
    if auth.enabled:
        smtp = auth.smtp
        origin = auth.origin
        if not auth.secure_cookies and auth.rp_id not in ("localhost", "127.0.0.1", "::1"):
            origin += " (passkeys need https or localhost)"
        lines += [
            f"auth.origin={origin}",
            f"auth.rp_id={auth.rp_id}",
            f"auth.rp_name={auth.rp_name}",
            f"auth.session_days={auth.session_days}",
            f"auth.registration_open={'on' if auth.registration_open else 'off'}",
            f"auth.secure_cookies={'on' if auth.secure_cookies else 'off'}",
            (f"auth.smtp={smtp.host}:{smtp.port} tls={smtp.tls} from={smtp.sender} "
             f"user={smtp.username or '(none)'} password={'set' if smtp.password else 'unset'}")
            if smtp.configured else "auth.smtp=(not configured: invite links are copied instead)",
        ]
    return lines


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

    def translate_path(self, path):
        target = auth.rewrite(urllib.parse.urlsplit(path).path)
        return super().translate_path(path if target is None else target)

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if auth.enabled():
            if auth.handle(self) or not auth.gate(self):
                return
            if path == "/play/":
                return self.redirect("/play")
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

    def do_POST(self):
        if not (auth.enabled() and auth.handle(self)):
            self.send_error(501, "Unsupported method (%r)" % self.command)

    do_PATCH = do_POST
    do_DELETE = do_POST

    def do_HEAD(self):
        if auth.enabled() and (auth.handle(self) or not auth.gate(self)):
            return
        super().do_HEAD()

    def redirect(self, location):
        self.send_response(301)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_PUT(self):
        if auth.enabled() and (auth.handle(self) or not auth.gate(self)):
            return
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


def main(argv=None):
    settings = resolve(argv)
    require_web_files(settings.dir, settings.auth.enabled)
    settings.db.parent.mkdir(parents=True, exist_ok=True)
    with contextlib.closing(sqlite3.connect(settings.db)) as conn:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.executescript(SCHEMA)
    auth.configure(settings.db, config=settings.auth)

    Handler.db_path = settings.db
    Handler.rom_path = settings.rom
    if ":" not in settings.bind:
        Server.address_family = socket.AF_INET
    handler = functools.partial(Handler, directory=str(settings.dir))
    with Server((settings.bind, settings.port), handler) as httpd:
        for line in describe(settings):
            print(f"config: {line}", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
