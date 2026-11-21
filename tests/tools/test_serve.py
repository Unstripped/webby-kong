"""tools/serve.py: the EEPROM save API that web/saves.js relies on, the mounted ROM web/main.js loads and the
co-op WebSocket relay web/net.js uses."""
import base64
import contextlib
import functools
import hashlib
import http.client
import json
import os
import socket
import sqlite3
import struct
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


class WsClient:
    """Minimal blocking RFC 6455 client: masked frames out, (opcode, payload) in."""

    def __init__(self, port, role):
        self.sock = socket.create_connection(("::1", port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {serve.NET_PATH}?role={role} HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
                          .encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("no handshake response")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode().split("\r\n")
        self.status = lines[0]
        headers = dict(line.split(": ", 1) for line in lines[1:])
        accept = base64.b64encode(hashlib.sha1((key + serve.WS_GUID).encode()).digest()).decode()
        if not self.status.startswith("HTTP/1.1 101") or headers.get("Sec-WebSocket-Accept") != accept:
            raise ConnectionError(f"bad handshake: {head!r}")

    def read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def send_frame(self, opcode, payload):
        n = len(payload)
        if n < 126:
            head = struct.pack("!BB", 0x80 | opcode, 0x80 | n)
        elif n < 1 << 16:
            head = struct.pack("!BBH", 0x80 | opcode, 0xFE, n)
        else:
            head = struct.pack("!BBQ", 0x80 | opcode, 0xFF, n)
        mask = os.urandom(4)
        self.sock.sendall(head + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))

    def send(self, data):
        self.send_frame(2, data)

    def recv_frame(self):
        b0, b1 = self.read(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack("!H", self.read(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self.read(8))[0]
        assert not b1 & 0x80, "server frames are unmasked"
        return b0 & 0x0F, self.read(n)

    def recv(self):
        """Next data message; fails on a close frame."""
        opcode, payload = self.recv_frame()
        assert opcode == 2, (opcode, payload)
        return payload

    def recv_ctrl(self):
        data = self.recv()
        assert data[:2] == b"\0\0", data
        return json.loads(data[2:])

    def recv_close(self):
        """-> close code; answers the close like a browser."""
        opcode, payload = self.recv_frame()
        assert opcode == 8, (opcode, payload)
        with contextlib.suppress(OSError):  # the relay may already have dropped the connection
            self.send_frame(8, payload[:2])
        return struct.unpack("!H", payload[:2])[0]

    def close(self):
        with contextlib.suppress(OSError):
            self.send_frame(8, struct.pack("!H", 1000))
        self.sock.close()


class NetRelayTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        tmp = Path(self._tmp.name)

        class QuietHandler(serve.Handler):
            db_path = tmp / "saves.sqlite3"

            def log_message(self, *args):
                pass

        self.server = serve.Server(("::1", 0), functools.partial(QuietHandler, directory=str(tmp)))
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.clients = []

    def tearDown(self):
        for c in self.clients:
            c.close()
        self.server.shutdown()
        self.server.server_close()
        self._tmp.cleanup()

    def connect(self, role):
        c = WsClient(self.port, role)
        self.clients.append(c)
        return c

    def host_with_joiners(self, n):
        host = self.connect("host")
        joiners = []
        for _ in range(n):
            j = self.connect("join")
            msg = host.recv_ctrl()
            self.assertEqual(msg["t"], "peer-join")
            j.id = msg["id"]
            joiners.append(j)
        return host, joiners

    def test_routing_rewrites_byte0(self):
        host, (a, b) = self.host_with_joiners(2)
        self.assertNotEqual(a.id, b.id)
        self.assertTrue(1 <= a.id <= 255 and 1 <= b.id <= 255)
        # joiner -> host: byte 0 becomes the sender's id
        a.send(bytes([77, 1]) + struct.pack("<II", 5, 0x12345678))
        self.assertEqual(host.recv(), bytes([a.id, 1]) + struct.pack("<II", 5, 0x12345678))
        b.send(bytes([0, 3, 9]))
        self.assertEqual(host.recv(), bytes([b.id, 3, 9]))
        # host -> one joiner
        host.send(bytes([b.id, 0]) + b'{"t":"ready"}')
        self.assertEqual(b.recv(), bytes([b.id, 0]) + b'{"t":"ready"}')
        # host -> everyone (76802 bytes: the 64-bit length form); a's first message is this one, so the unicast
        # above reached only b
        big = bytes([0, 4]) + bytes(range(256)) * 300
        host.send(big)
        self.assertEqual(a.recv(), big)
        self.assertEqual(b.recv(), big)
        # a 16-bit length form message
        mid = bytes([0, 2]) + bytes(1000)
        host.send(mid)
        self.assertEqual(a.recv(), mid)
        a.close()
        self.assertEqual(host.recv_ctrl(), {"t": "peer-leave", "id": a.id})

    def test_second_host_rejected(self):
        self.connect("host")
        self.assertEqual(self.connect("host").recv_close(), 4001)

    def test_join_without_host_rejected(self):
        self.assertEqual(self.connect("join").recv_close(), 4002)

    def test_sixth_client_rejected(self):
        self.host_with_joiners(4)
        self.assertEqual(self.connect("join").recv_close(), 4003)

    def test_host_disconnect_ends_session(self):
        host, (a, b) = self.host_with_joiners(2)
        host.close()
        for j in (a, b):
            self.assertEqual(j.recv_ctrl(), {"t": "end", "reason": "host left"})
            self.assertEqual(j.recv_close(), 1000)
        # the room is free again
        host = self.connect("host")
        self.connect("join")
        self.assertEqual(host.recv_ctrl()["t"], "peer-join")

    def test_oversized_message_closed_1009(self):
        host = self.connect("host")
        # header only: the relay refuses on the length before reading a mask or payload
        host.sock.sendall(struct.pack("!BBQ", 0x82, 0xFF, serve.WS_MAX_MESSAGE + 1))
        self.assertEqual(host.recv_close(), 1009)


if __name__ == "__main__":
    unittest.main()
