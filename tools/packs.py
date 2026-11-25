"""Community audio packs for tools/serve.py: zips of replacement songs and sound effects, shared by every user.

A pack is a zip holding a root manifest.json plus audio files (format: docs/audio-packs.md). Uploads are validated
here, stored as <pack dir>/<id>.zip next to the database (/data/packs in the Docker image) and listed for everyone.
The uploader or the server owner can delete a pack; packs are never edited (new content gets a new id). With the
sign-in gate off every request is anonymous: uploader is null and anyone can delete.

  GET    /api/packs             {"packs": [pack...]} (newest first)
  POST   /api/packs             raw zip body (at most 256 MiB) -> 201 {"pack"}; 400 bad_pack, 409 duplicate,
                                413 too_large
  GET    /api/packs/<id>/file   the zip (application/zip, immutable)
  DELETE /api/packs/<id>        204; 403 not_yours unless the uploader or the owner asks

pack = {"id", "name", "author", "version", "description", "songs", "sfx", "size", "sha1", "created_at",
"uploader": username | null, "can_delete": bool}. Errors are JSON {"error": "<code>", "message": "<sentence>"}.
"""
import contextlib
import hashlib
import json
import math
import os
import re
import shutil
import sqlite3
import uuid
import zipfile
from pathlib import Path
from urllib.parse import urlsplit

from saves import ApiError, _json, _length, _no_content

API = "/api/packs"
MAX_PACK_BYTES = 256 * 1024 * 1024
MAX_UNPACKED_BYTES = 512 * 1024 * 1024
MAX_ENTRIES = 2000
MAX_MANIFEST_BYTES = 64 * 1024
AUDIO_EXT = (".ogg", ".oga", ".opus", ".mp3", ".wav", ".flac")
CHUNK = 1024 * 1024
SONG_MAX = 175
SFX_MAX = 0x1FFF
TEXT_FIELDS = (("author", 60), ("version", 20), ("description", 500))

SCHEMA = """
CREATE TABLE IF NOT EXISTS packs (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,  -- never reused: browsers enable packs by id
  user_id     INTEGER REFERENCES users(id) ON DELETE CASCADE,
  name        TEXT    NOT NULL,
  author      TEXT    NOT NULL,
  version     TEXT    NOT NULL,
  description TEXT    NOT NULL,
  songs       INTEGER NOT NULL,
  sfx         INTEGER NOT NULL,
  size        INTEGER NOT NULL,
  sha1        TEXT    NOT NULL UNIQUE,
  created_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
CREATE INDEX IF NOT EXISTS packs_user ON packs(user_id);
"""


def pack_dir(db_path):
    return Path(db_path).parent / "packs"


def init(db_path):
    """Create the table and the pack directory; drop interrupted uploads and files whose row is gone (a user
    deletion cascades to their packs' rows but cannot remove the files)."""
    directory = pack_dir(db_path)
    directory.mkdir(parents=True, exist_ok=True)
    with contextlib.closing(sqlite3.connect(db_path)) as conn:
        conn.executescript(SCHEMA)
        conn.commit()
        ids = {row[0] for row in conn.execute("SELECT id FROM packs")}
    for part in directory.glob("*.part"):
        part.unlink(missing_ok=True)
    for path in directory.glob("*.zip"):
        if path.stem.isdigit() and int(path.stem) not in ids:
            path.unlink(missing_ok=True)


def is_api(path):
    return path == API or path.startswith(API + "/")


# --- validation -------------------------------------------------------------------------------------------------

def _bad(message):
    return ApiError(400, "bad_pack", message)


def _unsafe_name(name):
    return name.startswith("/") or "\\" in name or ".." in name.split("/")


def _is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def _parse_id(key, maximum, minimum):
    if re.fullmatch(r"[0-9]+", key):
        value = int(key)
    elif re.fullmatch(r"0[xX][0-9a-fA-F]+", key):
        value = int(key[2:], 16)
    else:
        return None
    return value if minimum <= value <= maximum else None


def _entry(kind, ident, value, files):
    where = f"{kind} 0x{ident:X}"
    if not isinstance(value, dict):
        raise _bad(f"{where}: entry is invalid")
    file = value.get("file")
    if not isinstance(file, str) or not file:
        raise _bad(f"{where}: file is invalid")
    if file not in files:
        raise _bad(f'{where}: file "{file}" is not in the pack')
    if not file.lower().endswith(AUDIO_EXT):
        raise _bad(f"{where}: file must be .ogg, .oga, .opus, .mp3, .wav or .flac")
    if "loop" in value and not isinstance(value["loop"], bool):
        raise _bad(f"{where}: loop is invalid")
    start = value.get("loopStart", 0)
    if not _is_number(start) or start < 0:
        raise _bad(f"{where}: loopStart is invalid")
    if "loopEnd" in value and (not _is_number(value["loopEnd"]) or value["loopEnd"] <= start):
        raise _bad(f"{where}: loopEnd is invalid")
    gain = value.get("gain", 1)
    if not _is_number(gain) or not 0 < gain <= 4:
        raise _bad(f"{where}: gain is invalid")


def _table(manifest, kind, maximum, minimum, message, files):
    table = manifest.get(kind, {})
    if not isinstance(table, dict):
        raise _bad(f"{kind} must be an object")
    seen = set()
    for key, value in table.items():
        ident = _parse_id(key, maximum, minimum)
        if ident is None:
            raise _bad(f'{kind} key "{key}" is not {message}')
        if ident in seen:
            raise _bad(f"{kind} id 0x{ident:X} is listed twice")
        seen.add(ident)
        _entry(kind, ident, value, files)
    return len(table)


def parse_manifest(manifest, files):
    """The manifest rules of docs/audio-packs.md -> ({name, author, version, description}, songs, sfx)."""
    if not isinstance(manifest, dict):
        raise _bad("manifest.json must be a JSON object")
    if manifest.get("format") != 1 or isinstance(manifest.get("format"), bool):
        raise _bad("format must be 1")
    name = manifest.get("name")
    if not isinstance(name, str) or not 1 <= len(name.strip()) <= 60:
        raise _bad("name must be 1-60 characters")
    meta = {"name": name.strip()}
    for field, limit in TEXT_FIELDS:
        value = manifest.get(field, "")
        if not isinstance(value, str) or len(value.strip()) > limit:
            raise _bad(f"{field} must be a string of at most {limit} characters")
        meta[field] = value.strip()
    songs = _table(manifest, "songs", SONG_MAX, 0, "a song id (0-175 or 0x0-0xAF)", files)
    sfx = _table(manifest, "sfx", SFX_MAX, 1, "a sound id (1-8191 or 0x1-0x1FFF)", files)
    if songs + sfx == 0:
        raise _bad("the pack replaces nothing (no songs or sfx)")
    return meta, songs, sfx


def _reject_constant(name):
    raise ValueError(name)


def validate(path):
    """Check a stored upload against the pack format -> ({name, author, version, description}, songs, sfx);
    ApiError 400 bad_pack otherwise."""
    try:
        zf = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, ValueError, EOFError):
        raise _bad("not a zip file") from None
    with zf:
        infos = zf.infolist()
        if len(infos) > MAX_ENTRIES:
            raise _bad(f"too many entries (at most {MAX_ENTRIES})")
        total = 0
        for info in infos:
            if _has_zip64(info):
                raise _bad("zip64 packs are not supported")
            if _unsafe_name(info.filename):
                raise _bad(f'entry "{info.filename}" has an unsafe name')
            if info.flag_bits & 1:
                raise _bad(f'entry "{info.filename}" is encrypted')
            if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                raise _bad(f'entry "{info.filename}" uses an unsupported compression method')
            total += info.file_size
        if total > MAX_UNPACKED_BYTES:
            raise _bad("the pack unpacks to more than 512 MiB")
        try:
            corrupt = zf.testzip()
        except Exception:  # zlib.error, EOFError, ...: testzip only maps BadZipFile to a name
            raise _bad("corrupt entry") from None
        if corrupt is not None:
            raise _bad(f"corrupt entry {corrupt}")
        files = {info.filename for info in infos if not info.is_dir()}
        if "manifest.json" not in files:
            raise _bad("no manifest.json at the root of the pack")
        if zf.getinfo("manifest.json").file_size > MAX_MANIFEST_BYTES:
            raise _bad("manifest.json is larger than 64 KiB")
        try:
            manifest = json.loads(zf.read("manifest.json").decode("utf-8-sig"), parse_constant=_reject_constant)
        except (UnicodeDecodeError, ValueError):
            raise _bad("manifest.json is not valid UTF-8 JSON") from None
    return parse_manifest(manifest, files)


def _has_zip64(info):
    """Whether the central directory entry carries a zip64 extra field (header id 0x0001)."""
    extra, pos = info.extra, 0
    while pos + 4 <= len(extra):
        header, size = int.from_bytes(extra[pos:pos + 2], "little"), int.from_bytes(extra[pos + 2:pos + 4], "little")
        if header == 0x0001:
            return True
        pos += 4 + size
    return False


# --- endpoints --------------------------------------------------------------------------------------------------

def _is_owner(ctx):
    if not ctx.auth_on or ctx.user_id is None:
        return False
    row = ctx.conn.execute("SELECT is_owner FROM users WHERE id = ?", (ctx.user_id,)).fetchone()
    return bool(row and row["is_owner"])


def pack_json(ctx, row, is_owner):
    can_delete = True if not ctx.auth_on else (row["user_id"] == ctx.user_id or is_owner)
    return {key: row[key] for key in ("id", "name", "author", "version", "description", "songs", "sfx", "size",
                                      "sha1", "created_at")} | {
        "uploader": row["uploader"] if ctx.auth_on else None, "can_delete": bool(can_delete)}


def _select(ctx, where="", params=()):
    if ctx.auth_on:
        sql = ("SELECT p.*, u.username AS uploader FROM packs p LEFT JOIN users u ON u.id = p.user_id "
               f"{where} ORDER BY p.id DESC")
    else:
        sql = f"SELECT p.*, NULL AS uploader FROM packs p {where} ORDER BY p.id DESC"
    return ctx.conn.execute(sql, params).fetchall()


def _pack(ctx, pack_id):
    rows = _select(ctx, "WHERE p.id = ?", (int(pack_id),))
    if not rows:
        raise ApiError(404, "not_found", "no such pack")
    return rows[0]


def _list(ctx, match):
    is_owner = _is_owner(ctx)
    _json(ctx.handler, 200, {"packs": [pack_json(ctx, row, is_owner) for row in _select(ctx)]})


def _receive(handler, length, part):
    """Stream the request body into `part` -> its SHA-1 hex digest."""
    digest = hashlib.sha1()
    left = length
    with open(part, "wb") as f:
        while left:
            chunk = handler.rfile.read(min(CHUNK, left))
            if not chunk:
                handler.close_connection = True
                raise ApiError(400, "request_invalid", "the body is shorter than Content-Length")
            digest.update(chunk)
            f.write(chunk)
            left -= len(chunk)
    return digest.hexdigest()


def _upload(ctx, match):
    handler, conn = ctx.handler, ctx.conn
    length = _length(handler)
    if length <= 0:
        raise ApiError(400, "request_invalid", "empty body")
    if length > MAX_PACK_BYTES:
        handler.close_connection = True
        raise ApiError(413, "too_large", "packs are limited to 256 MiB")
    part = ctx.directory / f"upload-{uuid.uuid4().hex}.part"
    target = None
    try:
        try:
            sha1 = _receive(handler, length, part)
        except OSError:
            handler.close_connection = True
            raise ApiError(500, "storage_error", "the pack could not be stored") from None
        meta, songs, sfx = validate(part)
        conn.execute("BEGIN IMMEDIATE")
        try:
            dup = conn.execute("SELECT id FROM packs WHERE sha1 = ?", (sha1,)).fetchone()
            if dup is not None:
                raise ApiError(409, "duplicate", f"already uploaded as pack {dup['id']}")
            cur = conn.execute("INSERT INTO packs (user_id, name, author, version, description, songs, sfx, size, "
                               "sha1) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                               (ctx.user_id, meta["name"], meta["author"], meta["version"], meta["description"],
                                songs, sfx, length, sha1))
            target = ctx.directory / f"{cur.lastrowid}.zip"
            # the file is in place before the row is visible; a failed commit leaves an orphan init() removes
            os.replace(part, target)
            conn.execute("COMMIT")
        except BaseException:
            conn.execute("ROLLBACK")
            if target is not None:
                target.unlink(missing_ok=True)
            raise
    finally:
        part.unlink(missing_ok=True)
    _json(handler, 201, {"pack": pack_json(ctx, _pack(ctx, cur.lastrowid), _is_owner(ctx))})


def _download(ctx, match):
    row = _pack(ctx, match["id"])
    path = ctx.directory / f"{row['id']}.zip"
    try:
        f = open(path, "rb")
    except FileNotFoundError:
        raise ApiError(404, "not_found", "no such pack") from None
    with f:
        handler = ctx.handler
        handler.send_response(200)
        handler.send_header("Content-Type", "application/zip")
        handler.send_header("Content-Length", str(os.fstat(f.fileno()).st_size))
        handler.send_header("Cache-Control", "private, max-age=31536000, immutable")
        handler.send_header("Content-Disposition", f'attachment; filename="pack-{row["id"]}.zip"')
        handler.end_headers()
        shutil.copyfileobj(f, handler.wfile)


def _delete(ctx, match):
    row = _pack(ctx, match["id"])
    if not pack_json(ctx, row, _is_owner(ctx))["can_delete"]:
        raise ApiError(403, "not_yours", "only the uploader or the owner can delete this pack")
    ctx.conn.execute("DELETE FROM packs WHERE id = ?", (row["id"],))
    (ctx.directory / f"{row['id']}.zip").unlink(missing_ok=True)
    _no_content(ctx.handler)


# --- routing ----------------------------------------------------------------------------------------------------

ROUTES = [
    ("GET", r"^/api/packs$", _list),
    ("POST", r"^/api/packs$", _upload),
    ("GET", r"^/api/packs/(?P<id>\d{1,18})/file$", _download),
    ("DELETE", r"^/api/packs/(?P<id>\d{1,18})$", _delete),
]
_COMPILED = [(method, re.compile(pattern), func) for method, pattern, func in ROUTES]


class _Context:
    def __init__(self, handler, conn, user_id, auth_on, directory):
        self.handler = handler
        self.conn = conn
        self.user_id = user_id
        self.auth_on = auth_on
        self.directory = directory


def _dispatch(ctx, path, method):
    for route_method, pattern, func in _COMPILED:
        if route_method != method:
            continue
        match = pattern.match(path)
        if match is not None:
            func(ctx, match.groupdict())
            return True
    return False


def handle(handler, db_path, user_id, auth_on):
    """Answer one /api/packs request for `user_id` (None: the sign-in gate is off)."""
    path = urlsplit(handler.path).path
    try:
        with contextlib.closing(sqlite3.connect(db_path, timeout=5, isolation_level=None)) as conn:
            conn.row_factory = sqlite3.Row
            ctx = _Context(handler, conn, user_id, bool(auth_on), pack_dir(db_path))
            if not _dispatch(ctx, path, handler.command.upper()):
                raise ApiError(404, "not_found", "no such endpoint")
    except ApiError as err:
        _json(handler, err.status, {"error": err.code, "message": err.message})
    except sqlite3.Error:
        _json(handler, 500, {"error": "storage_error", "message": "the database is unhappy"})
