"""Save games for tools/serve.py: each one a 2 KB EEPROM with its whole version history, owned by a user.

A save game is one EEPROM image (DK64's own file slots live inside it) plus every version the game ever uploaded.
With the sign-in gate on, saves belong to the signed-in user (saves.user_id); with it off there is one anonymous
owner (user_id NULL), whose saves the owner account adopts when the gate comes on (tools/auth/db.py). Another user's
save answers 404 exactly like a missing one.

  GET    /api/saves                 {"saves": [save...] (most recently played first), "dashboard": bool}
  POST   /api/saves                 {"name"?, "eeprom"?} -> 201 {"save"}; no name -> "Save N"; eeprom (base64 of 2048
                                    bytes) becomes the save's first version (mtime = now)
  GET    /api/saves/<id>            {"save", "dashboard"}
  PATCH  /api/saves/<id>            {"name"} -> {"save"}
  DELETE /api/saves/<id>            204; the save and its EEPROM history are gone
  GET    /api/saves/<id>/eeprom     latest version (highest client mtime): 2048 bytes, X-Save-Mtime / X-Save-Sha1;
                                    404 if none
  PUT    /api/saves/<id>/eeprom     2048-byte body plus X-Save-Mtime (ms since epoch) and an optional X-Save-Summary
                                    (JSON object of SUMMARY_KEYS, from the game); stored unless it repeats this save's
                                    latest version or an identical (sha1, mtime) row; played_at/summary always update
  PUT    /api/saves/<id>/resume     {"resume": null | {RESUME_KEYS...}}: where "Exit to dashboard" left the game

save = {"id", "name", "created_at", "played_at", "summary": object | null, "resume": object | null}. Errors are JSON
{"error": "<code>", "message": "<sentence>"}, like tools/auth/routes.py.

Recovery with the sqlite3 CLI (DB defaults to ~/.local/share/webby-kong/saves.sqlite3, S is a save id):

  list saves           sqlite3 DB "select id, user_id, name, played_at from saves order by id"
  list versions        sqlite3 DB "select id, mtime_ms, received_at, sha1 from eeprom where save_id=S order by id"
  export version N     sqlite3 DB "select writefile('x.eep', bytes) from eeprom where id=N"
  make N current       sqlite3 DB "insert into eeprom (mtime_ms, sha1, bytes, save_id) select
                         cast(strftime('%s','now') as integer)*1000, sha1, bytes, save_id from eeprom where id=N"
                       (the newest mtime wins the next time that save is played)
"""
import base64
import binascii
import contextlib
import hashlib
import json
import math
import re
import sqlite3
import time
from datetime import datetime, timezone
from urllib.parse import urlsplit

EEPROM_SIZE = 2048
API = "/api/saves"
NAME_MAX = 40
MAX_JSON_BYTES = 4096
SUMMARY_KEYS = ("slot", "percent", "goldenBananas", "keys", "crowns", "medals", "fairies", "blueprints", "kongs",
                "playSeconds")
RESUME_KEYS = ("slot", "kong", "map", "exit", "hasPos", "x", "y", "z", "yaw")
ORDER = "ORDER BY played_at IS NULL, played_at DESC, id DESC"

SCHEMA = """
CREATE TABLE IF NOT EXISTS saves (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,  -- never reused: browsers key files and quicksaves by it
  user_id    INTEGER REFERENCES users(id) ON DELETE CASCADE,
  name       TEXT    NOT NULL,
  created_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  played_at  TEXT,
  summary    TEXT,
  resume     TEXT);
CREATE INDEX IF NOT EXISTS saves_user ON saves(user_id);
CREATE TABLE IF NOT EXISTS eeprom (
  id          INTEGER PRIMARY KEY,
  mtime_ms    INTEGER NOT NULL,
  received_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  sha1        TEXT    NOT NULL,
  bytes       BLOB    NOT NULL,
  save_id     INTEGER REFERENCES saves(id) ON DELETE CASCADE);
"""


def init(path):
    """Create the tables, and move a pre-saves database's single shared history into a save called "Save 1"."""
    with contextlib.closing(sqlite3.connect(path)) as conn:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.executescript(SCHEMA)
        columns = [row[1] for row in conn.execute("PRAGMA table_info(eeprom)")]
        if "save_id" not in columns:
            conn.execute("ALTER TABLE eeprom ADD COLUMN save_id INTEGER REFERENCES saves(id) ON DELETE CASCADE")
        conn.execute("CREATE INDEX IF NOT EXISTS eeprom_save ON eeprom(save_id, mtime_ms)")
        conn.execute("DROP INDEX IF EXISTS eeprom_mtime")
        played_at = conn.execute("SELECT max(received_at) FROM eeprom WHERE save_id IS NULL").fetchone()[0]
        if played_at is not None:
            cur = conn.execute("INSERT INTO saves (user_id, name, played_at) VALUES (NULL, 'Save 1', ?)",
                               (played_at,))
            conn.execute("UPDATE eeprom SET save_id = ? WHERE save_id IS NULL", (cur.lastrowid,))
        conn.commit()


def is_api(path):
    return path == API or path.startswith(API + "/")


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:23] + "Z"


class ApiError(Exception):
    def __init__(self, status, code, message):
        super().__init__(code)
        self.status = status
        self.code = code
        self.message = message


NOT_FOUND = (404, "not_found", "no such save")


# --- responses --------------------------------------------------------------------------------------------------

def _send(handler, status, body=b"", headers=()):
    handler.send_response(status)
    for name, value in headers:
        handler.send_header(name, value)
    if status != 204:
        handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    if handler.command != "HEAD" and body:
        handler.wfile.write(body)


def _json(handler, status, payload):
    _send(handler, status, json.dumps(payload, separators=(",", ":")).encode("utf-8"),
          [("Content-Type", "application/json"), ("Cache-Control", "no-store")])


def _no_content(handler):
    _send(handler, 204, headers=[("Cache-Control", "no-store")])


# --- request helpers --------------------------------------------------------------------------------------------

def _length(handler):
    try:
        return int(handler.headers.get("Content-Length") or 0)
    except ValueError:
        raise ApiError(400, "request_invalid", "Content-Length must be an integer") from None


def _body(handler):
    size = _length(handler)
    if size < 0 or size > MAX_JSON_BYTES:
        raise ApiError(400, "request_invalid", "request body too large")
    if size == 0:
        return {}
    content_type = (handler.headers.get("Content-Type") or "").split(";")[0].strip().lower()
    data = handler.rfile.read(size)
    if content_type != "application/json":
        raise ApiError(400, "request_invalid", "JSON requests only")
    try:
        payload = json.loads(data)
    except (ValueError, UnicodeDecodeError):
        raise ApiError(400, "request_invalid", "body is not JSON") from None
    if not isinstance(payload, dict):
        raise ApiError(400, "request_invalid", "body must be a JSON object")
    return payload


def _name(value):
    """A stripped save name, or ApiError: 1-NAME_MAX printable characters."""
    name = value.strip() if isinstance(value, str) else None
    if not name or len(name) > NAME_MAX or not name.isprintable():
        raise ApiError(400, "name_invalid", f"save names are 1-{NAME_MAX} printable characters")
    return name


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _summary(header):
    """X-Save-Summary -> the JSON text to store, or None when the header is absent."""
    if header is None or not header.strip():
        return None
    try:
        summary = json.loads(header)
    except ValueError:
        summary = None
    if (not isinstance(summary, dict) or set(summary) != set(SUMMARY_KEYS)
            or not all(_is_int(summary[k]) and 0 <= summary[k] <= 10**9 for k in SUMMARY_KEYS)):
        raise ApiError(400, "summary_invalid",
                       "X-Save-Summary must be a JSON object of integers: " + ", ".join(SUMMARY_KEYS))
    return json.dumps({k: summary[k] for k in SUMMARY_KEYS}, separators=(",", ":"))


def _resume(value):
    """The resume point to store (JSON text) or None to clear it; ApiError when malformed."""
    if value is None:
        return None
    ok = isinstance(value, dict) and all(k in value for k in RESUME_KEYS)
    if ok:
        for k in RESUME_KEYS:
            v = value[k]
            if k in ("x", "y", "z"):
                ok = ok and (_is_int(v) or (isinstance(v, float) and math.isfinite(v)))
            else:
                ok = ok and _is_int(v)
    if not ok:
        raise ApiError(400, "resume_invalid", "resume must be null or an object with " + ", ".join(RESUME_KEYS))
    stored = {k: value[k] for k in RESUME_KEYS}
    stored["time"] = now_iso()
    return json.dumps(stored, separators=(",", ":"))


def _eeprom(value):
    """POST /api/saves "eeprom": base64 of exactly EEPROM_SIZE bytes -> the bytes, or ApiError."""
    error = ApiError(400, "eeprom_invalid", f"eeprom must be {EEPROM_SIZE} bytes, base64-encoded")
    if not isinstance(value, str):
        raise error
    try:
        data = base64.b64decode(value, validate=True)
    except binascii.Error:
        raise error from None
    if len(data) != EEPROM_SIZE:
        raise error
    return data


def save_json(row):
    return {
        "id": row["id"],
        "name": row["name"],
        "created_at": row["created_at"],
        "played_at": row["played_at"],
        "summary": json.loads(row["summary"]) if row["summary"] else None,
        "resume": json.loads(row["resume"]) if row["resume"] else None,
    }


def _save(conn, save_id, user_id):
    row = conn.execute("SELECT * FROM saves WHERE id = ? AND user_id IS ?", (int(save_id), user_id)).fetchone()
    if row is None:
        raise ApiError(*NOT_FOUND)
    return row


# --- endpoints --------------------------------------------------------------------------------------------------

def _list(ctx, match):
    rows = ctx.conn.execute(f"SELECT * FROM saves WHERE user_id IS ? {ORDER}", (ctx.user_id,)).fetchall()
    _json(ctx.handler, 200, {"saves": [save_json(r) for r in rows], "dashboard": ctx.dashboard})


def _create(ctx, match):
    body = _body(ctx.handler)
    raw = body.get("name")
    if raw is None or (isinstance(raw, str) and not raw.strip()):
        count = ctx.conn.execute("SELECT count(*) FROM saves WHERE user_id IS ?", (ctx.user_id,)).fetchone()[0]
        name = f"Save {count + 1}"
    else:
        name = _name(raw)
    eeprom = None if body.get("eeprom") is None else _eeprom(body["eeprom"])
    conn = ctx.conn
    conn.execute("BEGIN IMMEDIATE")
    try:
        cur = conn.execute("INSERT INTO saves (user_id, name) VALUES (?, ?)", (ctx.user_id, name))
        if eeprom is not None:
            conn.execute("INSERT INTO eeprom (mtime_ms, sha1, bytes, save_id) VALUES (?, ?, ?, ?)",
                         (int(time.time() * 1000), hashlib.sha1(eeprom).hexdigest(), eeprom, cur.lastrowid))
        conn.execute("COMMIT")
    except BaseException:
        conn.execute("ROLLBACK")
        raise
    _json(ctx.handler, 201, {"save": save_json(_save(conn, cur.lastrowid, ctx.user_id))})


def _get(ctx, match):
    _json(ctx.handler, 200, {"save": save_json(_save(ctx.conn, match["id"], ctx.user_id)),
                             "dashboard": ctx.dashboard})


def _rename(ctx, match):
    row = _save(ctx.conn, match["id"], ctx.user_id)
    name = _name(_body(ctx.handler).get("name"))
    ctx.conn.execute("UPDATE saves SET name = ? WHERE id = ?", (name, row["id"]))
    _json(ctx.handler, 200, {"save": save_json(_save(ctx.conn, row["id"], ctx.user_id))})


def _delete(ctx, match):
    row = _save(ctx.conn, match["id"], ctx.user_id)
    ctx.conn.execute("BEGIN IMMEDIATE")
    try:
        ctx.conn.execute("DELETE FROM eeprom WHERE save_id = ?", (row["id"],))
        ctx.conn.execute("DELETE FROM saves WHERE id = ?", (row["id"],))
        ctx.conn.execute("COMMIT")
    except BaseException:
        ctx.conn.execute("ROLLBACK")
        raise
    _no_content(ctx.handler)


def _get_eeprom(ctx, match):
    row = _save(ctx.conn, match["id"], ctx.user_id)
    latest = ctx.conn.execute("SELECT mtime_ms, sha1, bytes FROM eeprom WHERE save_id = ? "
                              "ORDER BY mtime_ms DESC, id DESC LIMIT 1", (row["id"],)).fetchone()
    if latest is None:
        raise ApiError(404, "no_save_data", "this save has no EEPROM data yet")
    _send(ctx.handler, 200, latest["bytes"], [
        ("Content-Type", "application/octet-stream"), ("Cache-Control", "no-store"),
        ("X-Save-Mtime", str(latest["mtime_ms"])), ("X-Save-Sha1", latest["sha1"])])


def _put_eeprom(ctx, match):
    handler = ctx.handler
    row = _save(ctx.conn, match["id"], ctx.user_id)
    length = _length(handler)
    if length != EEPROM_SIZE:
        raise ApiError(400, "request_invalid", f"body must be {EEPROM_SIZE} bytes")
    body = handler.rfile.read(length)
    mtime_header = handler.headers.get("X-Save-Mtime", "")
    if len(body) != EEPROM_SIZE or not mtime_header.isascii() or not mtime_header.isdigit():
        raise ApiError(400, "request_invalid", "X-Save-Mtime must be a non-negative integer")
    summary = _summary(handler.headers.get("X-Save-Summary"))
    mtime = int(mtime_header)
    sha1 = hashlib.sha1(body).hexdigest()
    conn = ctx.conn
    conn.execute("BEGIN IMMEDIATE")
    try:
        _save(conn, row["id"], ctx.user_id)  # again under the write lock: a DELETE may have run while the body arrived
        latest = conn.execute("SELECT sha1 FROM eeprom WHERE save_id = ? ORDER BY mtime_ms DESC, id DESC LIMIT 1",
                              (row["id"],)).fetchone()
        seen = conn.execute("SELECT 1 FROM eeprom WHERE save_id = ? AND sha1 = ? AND mtime_ms = ?",
                            (row["id"], sha1, mtime)).fetchone()
        if (latest is None or latest["sha1"] != sha1) and seen is None:
            conn.execute("INSERT INTO eeprom (mtime_ms, sha1, bytes, save_id) VALUES (?, ?, ?, ?)",
                         (mtime, sha1, body, row["id"]))
        conn.execute("UPDATE saves SET played_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'), "
                     "summary = COALESCE(?, summary) WHERE id = ?", (summary, row["id"]))
        conn.execute("COMMIT")
    except BaseException:
        conn.execute("ROLLBACK")
        raise
    _no_content(handler)


def _put_resume(ctx, match):
    row = _save(ctx.conn, match["id"], ctx.user_id)
    body = _body(ctx.handler)
    if "resume" not in body:
        raise ApiError(400, "resume_invalid", "the body needs a resume key (an object or null)")
    ctx.conn.execute("UPDATE saves SET resume = ? WHERE id = ?", (_resume(body["resume"]), row["id"]))
    _no_content(ctx.handler)


# --- routing ----------------------------------------------------------------------------------------------------

ROUTES = [
    ("GET", r"^/api/saves$", _list),
    ("POST", r"^/api/saves$", _create),
    ("GET", r"^/api/saves/(?P<id>\d{1,18})$", _get),
    ("PATCH", r"^/api/saves/(?P<id>\d{1,18})$", _rename),
    ("DELETE", r"^/api/saves/(?P<id>\d{1,18})$", _delete),
    ("GET", r"^/api/saves/(?P<id>\d{1,18})/eeprom$", _get_eeprom),
    ("PUT", r"^/api/saves/(?P<id>\d{1,18})/eeprom$", _put_eeprom),
    ("PUT", r"^/api/saves/(?P<id>\d{1,18})/resume$", _put_resume),
]
_COMPILED = [(method, re.compile(pattern), func) for method, pattern, func in ROUTES]


class _Context:
    def __init__(self, handler, conn, user_id, dashboard):
        self.handler = handler
        self.conn = conn
        self.user_id = user_id
        self.dashboard = dashboard


def _dispatch(ctx, path, method):
    for route_method, pattern, func in _COMPILED:
        if route_method != method:
            continue
        match = pattern.match(path)
        if match is not None:
            func(ctx, match.groupdict())
            return True
    return False


def handle(handler, db_path, user_id, dashboard):
    """Answer one /api/saves request for `user_id` (None: the sign-in gate is off)."""
    path = urlsplit(handler.path).path
    try:
        with contextlib.closing(sqlite3.connect(db_path, timeout=5, isolation_level=None)) as conn:
            conn.row_factory = sqlite3.Row
            if not _dispatch(_Context(handler, conn, user_id, bool(dashboard)), path, handler.command.upper()):
                raise ApiError(404, "not_found", "no such endpoint")
    except ApiError as err:
        _json(handler, err.status, {"error": err.code, "message": err.message})
    except sqlite3.Error:
        _json(handler, 500, {"error": "storage_error", "message": "the database is unhappy"})
