#!/usr/bin/env python3
"""Serve build/web, keep every version of the game's EEPROM save in SQLite, serve a mounted ROM and relay co-op.

Static files are served like `python3 -m http.server -d build/web 8080`. web/saves.js, web/main.js and web/net.js add:

  GET /api/eeprom   latest save (highest client mtime): 2048 bytes, X-Save-Mtime / X-Save-Sha1 headers; 404 if none
  PUT /api/eeprom   2048-byte body plus an X-Save-Mtime header (ms since epoch); stored unless it repeats the
                    latest version or an identical (sha1, mtime) row
  GET /api/rom      the --rom file (or the first *.z64/*.n64/*.v64 in the --rom directory), looked up per request;
                    404 if none. web/main.js starts it without the file picker (the Docker image mounts /rom)
  GET /api/net?role=host|join
                    WebSocket (RFC 6455) relay for online co-op (web/net.js): one room per server, one host and up
                    to 4 joiners. Every message is binary `[u8 peer][u8 type][payload]`; the relay only routes:
                    joiner -> host with byte 0 replaced by the sender's peer id (1-255); host -> byte 0 = 0 to every
                    joiner, else to that joiner. The relay itself sends type-0 JSON (byte 0 = 0): to the host
                    {"t":"peer-join","id":n} / {"t":"peer-leave","id":n}; to joiners {"t":"end","reason":"host left"}
                    and a close when the host disconnects. Close codes: 4001 host already connected, 4002 no host,
                    4003 session full, 1009 message over 1 MiB.

Recovery with the sqlite3 CLI (DB defaults to ~/.local/share/webby-kong/saves.sqlite3):

  list versions        sqlite3 DB "select id, mtime_ms, received_at, sha1 from eeprom order by id"
  export version N     sqlite3 DB "select writefile('x.eep', bytes) from eeprom where id=N"
  make N current       sqlite3 DB "insert into eeprom (mtime_ms, sha1, bytes) select
                         cast(strftime('%s','now') as integer)*1000, sha1, bytes from eeprom where id=N"
                       (the newest mtime wins at the next page load)
"""
import argparse
import base64
import contextlib
import functools
import hashlib
import http.server
import json
import socket
import sqlite3
import struct
import threading
import urllib.parse
from pathlib import Path

API_PATH = "/api/eeprom"
ROM_PATH = "/api/rom"
NET_PATH = "/api/net"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_MAX_MESSAGE = 1 << 20
NET_MAX_JOINERS = 4
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


class WsClosed(Exception):
    """The WebSocket peer closed, dropped the connection or broke the protocol."""


def ws_unmask(data, mask):
    n = len(data)
    key = (mask * (n // 4 + 1))[:n]
    return (int.from_bytes(data, "little") ^ int.from_bytes(key, "little")).to_bytes(n, "little")


class WsPeer:
    """One server-side RFC 6455 connection: a frame reader and a locked frame writer (relay threads share it)."""

    def __init__(self, sock, rfile):
        self.sock = sock
        self.rfile = rfile
        self.id = 0  # joiner peer id 1..255; 0 for the host
        self.lock = threading.Lock()
        self.closing = False  # a close frame was sent: send nothing else

    def send_frame(self, opcode, payload=b""):
        n = len(payload)
        if n < 126:
            head = struct.pack("!BB", 0x80 | opcode, n)
        elif n < 1 << 16:
            head = struct.pack("!BBH", 0x80 | opcode, 126, n)
        else:
            head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
        with self.lock:
            if self.closing:
                return
            self.closing = opcode == 8
            try:
                self.sock.sendall(head + payload)
            except OSError:
                self.closing = True

    def send(self, data):
        self.send_frame(2, data)

    def send_json(self, obj):
        self.send(b"\0\0" + json.dumps(obj, separators=(",", ":")).encode())

    def close(self, code, reason=""):
        self.send_frame(8, struct.pack("!H", code) + reason.encode()[:123])

    def read_exact(self, n):
        data = self.rfile.read(n)
        if data is None or len(data) != n:
            raise WsClosed
        return data

    def fail(self, code, reason):
        self.close(code, reason)
        raise WsClosed

    def recv(self):
        """Next data message (text or binary) as bytes; answers ping and close frames itself."""
        message = None
        while True:
            b0, b1 = self.read_exact(2)
            opcode, fin = b0 & 0x0F, b0 & 0x80
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack("!H", self.read_exact(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self.read_exact(8))[0]
            if not b1 & 0x80 or b0 & 0x70:  # client frames are masked; no extension was negotiated
                self.fail(1002, "protocol error")
            if n > WS_MAX_MESSAGE or (opcode == 0 and message is not None and len(message) + n > WS_MAX_MESSAGE):
                self.fail(1009, "message too big")
            mask = self.read_exact(4)
            payload = ws_unmask(self.read_exact(n), mask)
            if opcode >= 8:
                if not fin or n > 125:
                    self.fail(1002, "protocol error")
                if opcode == 8:
                    self.send_frame(8, payload[:2])  # echo the status code
                    raise WsClosed
                if opcode == 9:
                    self.send_frame(10, payload)
                continue
            if opcode == 0 and message is not None:
                message += payload
            elif opcode in (1, 2) and message is None:
                message = bytearray(payload)
            else:
                self.fail(1002, "protocol error")
            if fin:
                return bytes(message)


class NetRoom:
    """The co-op session of web/net.js: one host and up to NET_MAX_JOINERS joiners; routes by message byte 0."""

    def __init__(self):
        self.lock = threading.Lock()
        self.host = None
        self.joiners = {}
        self.next_id = 1

    def admit(self, peer, role):
        """-> None, or (close code, reason) when the peer cannot enter."""
        with self.lock:
            if role == "host":
                if self.host is not None:
                    return 4001, "host already connected"
                self.host = peer
                return None
            if self.host is None:
                return 4002, "no host"
            if len(self.joiners) >= NET_MAX_JOINERS:
                return 4003, "session full"
            while self.next_id in self.joiners:
                self.next_id = self.next_id % 255 + 1
            peer.id = self.next_id
            self.next_id = self.next_id % 255 + 1
            self.joiners[peer.id] = peer
            host = self.host
        # before the joiner's first message is read, so the host always learns the id first
        host.send_json({"t": "peer-join", "id": peer.id})
        return None

    def leave(self, peer):
        with self.lock:
            if peer is self.host:
                self.host = None
                ended = list(self.joiners.values())
                self.joiners.clear()
                host = None
            elif self.joiners.get(peer.id) is peer:
                del self.joiners[peer.id]
                ended = []
                host = self.host
            else:
                return
        if host is not None:
            host.send_json({"t": "peer-leave", "id": peer.id})
        for joiner in ended:
            joiner.send_json({"t": "end", "reason": "host left"})
            joiner.close(1000, "host left")

    def route(self, peer, data):
        if len(data) < 2:
            return
        with self.lock:
            if peer is self.host:
                if data[0] == 0:
                    targets = list(self.joiners.values())
                else:
                    targets = [self.joiners[data[0]]] if data[0] in self.joiners else []
            elif self.joiners.get(peer.id) is peer:
                targets = [self.host]
                data = bytes([peer.id]) + data[1:]
            else:
                targets = []  # a joiner whose host already left
        for target in targets:
            target.send(data)


class Server(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6

    def server_bind(self):
        # dual stack, like `python3 -m http.server`: localhost over IPv4 and IPv6 is the same origin as before
        if self.address_family == socket.AF_INET6:
            with contextlib.suppress(Exception):
                self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        super().server_bind()

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.net_room = NetRoom()


class Handler(http.server.SimpleHTTPRequestHandler):
    db_path: Path
    rom_path: Path | None = None

    def db(self):
        return contextlib.closing(sqlite3.connect(self.db_path, timeout=5, isolation_level=None))

    def is_api(self):
        return urllib.parse.urlsplit(self.path).path == API_PATH

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        path = url.path
        if path == ROM_PATH:
            return self.send_rom()
        if path == NET_PATH:
            return self.handle_net(url.query)
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

    def handle_net(self, query):
        role = urllib.parse.parse_qs(query).get("role", [""])[0]
        key = self.headers.get("Sec-WebSocket-Key", "")
        if (role not in ("host", "join") or self.headers.get("Upgrade", "").lower() != "websocket" or not key
                or self.headers.get("Sec-WebSocket-Version") != "13"):
            return self.send_error(400, f"WebSocket upgrade to {NET_PATH}?role=host|join required")
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.protocol_version = "HTTP/1.1"  # browsers require an HTTP/1.1 101
        self.close_connection = True
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        peer = WsPeer(self.connection, self.rfile)
        room = self.server.net_room
        refused = room.admit(peer, role)
        if refused is not None:
            peer.close(*refused)
            # wait briefly for the client's close reply so it sees a clean close with our code
            self.connection.settimeout(2)
            with contextlib.suppress(WsClosed, OSError):
                while True:
                    peer.recv()
            return
        try:
            while True:
                room.route(peer, peer.recv())
        except (WsClosed, OSError):
            pass
        finally:
            room.leave(peer)

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
