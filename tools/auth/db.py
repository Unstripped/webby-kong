"""SQLite schema and the queries the auth routes need.

The tables live in the same database file as the save history (tools/serve.py --db), so one bind-mount of /data
persists both. They are created only when WEBBY_KONG_AUTH is on, which keeps an auth-less deployment byte-identical to
what it was before this feature.

Timestamps are the same UTC ISO-8601 text the saves table uses (strftime('%Y-%m-%dT%H:%M:%fZ','now')), so they sort
lexicographically and compare with now_iso() as plain strings.
"""
import contextlib
import hashlib
import secrets
import sqlite3
from datetime import datetime, timedelta, timezone

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
  id            INTEGER PRIMARY KEY,
  handle        BLOB    NOT NULL UNIQUE,
  username      TEXT    NOT NULL UNIQUE COLLATE NOCASE,
  is_owner      INTEGER NOT NULL DEFAULT 0,
  disabled      INTEGER NOT NULL DEFAULT 0,
  created_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  last_login_at TEXT);

CREATE TABLE IF NOT EXISTS credentials (
  id              TEXT    PRIMARY KEY,
  user_id         INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  public_key      BLOB    NOT NULL,
  sign_count      INTEGER NOT NULL DEFAULT 0,
  transports      TEXT,
  aaguid          TEXT,
  backup_eligible INTEGER NOT NULL DEFAULT 0,
  backup_state    INTEGER NOT NULL DEFAULT 0,
  name            TEXT    NOT NULL DEFAULT 'Passkey',
  created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  last_used_at    TEXT);
CREATE INDEX IF NOT EXISTS credentials_user ON credentials(user_id);

CREATE TABLE IF NOT EXISTS sessions (
  id           TEXT    PRIMARY KEY,
  user_id      INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  expires_at   TEXT    NOT NULL,
  last_seen_at TEXT,
  user_agent   TEXT);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);

CREATE TABLE IF NOT EXISTS invites (
  id            INTEGER PRIMARY KEY,
  token_hash    TEXT    NOT NULL UNIQUE,
  created_by    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  note          TEXT,
  created_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  expires_at    TEXT,
  max_uses      INTEGER,
  uses          INTEGER NOT NULL DEFAULT 0,
  revoked_at    TEXT,
  last_email_to TEXT);

CREATE TABLE IF NOT EXISTS webauthn_challenges (
  id         TEXT    PRIMARY KEY,
  challenge  TEXT    NOT NULL,
  kind       TEXT    NOT NULL,
  user_id    INTEGER REFERENCES users(id) ON DELETE CASCADE,
  username   TEXT,
  handle     BLOB,
  invite_id  INTEGER REFERENCES invites(id) ON DELETE CASCADE,
  created_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
CREATE INDEX IF NOT EXISTS challenges_created ON webauthn_challenges(created_at);

CREATE TABLE IF NOT EXISTS settings (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL);
"""

CHALLENGE_TTL = timedelta(minutes=5)
REGISTRATION_OPEN = "registration_open"


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:23] + "Z"


def iso_from_now(**delta):
    return (datetime.now(timezone.utc) + timedelta(**delta)).strftime("%Y-%m-%dT%H:%M:%S.%f")[:23] + "Z"


def connect(path):
    conn = sqlite3.connect(path, timeout=5, isolation_level=None)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


@contextlib.contextmanager
def open_db(path):
    conn = connect(path)
    try:
        yield conn
    finally:
        conn.close()


def init(path, registration_open_default=True):
    """Create the auth tables in an existing (or new) saves database and seed the registration switch."""
    with contextlib.closing(sqlite3.connect(path)) as conn:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.executescript(SCHEMA)
        conn.execute(f"INSERT OR IGNORE INTO settings (key, value) VALUES ('{REGISTRATION_OPEN}', ?)",
                     ("1" if registration_open_default else "0",))
        conn.commit()


# --- settings ---------------------------------------------------------------------------------------------------

def get_setting(conn, key, default=None):
    row = conn.execute("SELECT value FROM settings WHERE key = ?", (key,)).fetchone()
    return row["value"] if row is not None else default


def set_setting(conn, key, value):
    conn.execute("INSERT INTO settings (key, value) VALUES (?, ?) "
                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value", (key, str(value)))


def registration_open(conn):
    return get_setting(conn, REGISTRATION_OPEN, "1") == "1"


# --- users ------------------------------------------------------------------------------------------------------

def user_json(row):
    return {
        "id": row["id"],
        "username": row["username"],
        "owner": bool(row["is_owner"]),
        "created_at": row["created_at"],
        "last_login_at": row["last_login_at"],
    }


def count_users(conn):
    return conn.execute("SELECT count(*) AS n FROM users").fetchone()["n"]


def get_user(conn, user_id):
    return conn.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()


def get_user_by_username(conn, username):
    return conn.execute("SELECT * FROM users WHERE username = ? COLLATE NOCASE", (username,)).fetchone()


def create_user(conn, username, handle=None):
    """Insert a user; the first one ever created is the owner. Returns the new row."""
    is_owner = 1 if count_users(conn) == 0 else 0
    cur = conn.execute("INSERT INTO users (handle, username, is_owner) VALUES (?, ?, ?)",
                       (handle or secrets.token_bytes(16), username, is_owner))
    return get_user(conn, cur.lastrowid)


def list_users(conn):
    return conn.execute(
        "SELECT u.*, (SELECT count(*) FROM credentials c WHERE c.user_id = u.id) AS passkey_count "
        "FROM users u ORDER BY u.id").fetchall()


# --- credentials ------------------------------------------------------------------------------------------------

def credential_json(row):
    return {
        "id": row["id"],
        "name": row["name"],
        "created_at": row["created_at"],
        "last_used_at": row["last_used_at"],
    }


def list_credentials(conn, user_id):
    return conn.execute("SELECT * FROM credentials WHERE user_id = ? ORDER BY created_at, id",
                        (user_id,)).fetchall()


def get_credential(conn, credential_id):
    return conn.execute("SELECT * FROM credentials WHERE id = ?", (credential_id,)).fetchone()


def create_credential(conn, user_id, credential_id, public_key, sign_count=0, transports=None,
                      aaguid=None, backup_eligible=False, backup_state=False, name="Passkey"):
    conn.execute(
        "INSERT INTO credentials (id, user_id, public_key, sign_count, transports, aaguid, backup_eligible, "
        "backup_state, name) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (credential_id, user_id, public_key, sign_count, transports, aaguid,
         int(bool(backup_eligible)), int(bool(backup_state)), name))


# --- challenges -------------------------------------------------------------------------------------------------

def create_challenge(conn, challenge, kind, user_id=None, username=None, handle=None, invite_id=None):
    """Store a WebAuthn challenge server-side and return the opaque id the client hands back."""
    conn.execute("DELETE FROM webauthn_challenges WHERE created_at < ?",
                 (iso_from_now(seconds=-int(CHALLENGE_TTL.total_seconds())),))
    challenge_id = secrets.token_urlsafe(18)
    conn.execute("INSERT INTO webauthn_challenges (id, challenge, kind, user_id, username, handle, invite_id) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?)",
                 (challenge_id, challenge, kind, user_id, username, handle, invite_id))
    return challenge_id


def take_challenge(conn, challenge_id, kind):
    """Delete and return a challenge (single use), or None when it is unknown, spent or too old."""
    row = conn.execute("SELECT * FROM webauthn_challenges WHERE id = ? AND kind = ?",
                       (challenge_id, kind)).fetchone()
    if row is None:
        return None
    conn.execute("DELETE FROM webauthn_challenges WHERE id = ?", (challenge_id,))
    if row["created_at"] < iso_from_now(seconds=-int(CHALLENGE_TTL.total_seconds())):
        return None
    return row


# --- invites ----------------------------------------------------------------------------------------------------

INVITE_SELECT = ("SELECT i.*, u.username AS created_by_username FROM invites i "
                 "LEFT JOIN users u ON u.id = i.created_by")


def token_hash(token):
    """What the database stores for a bearer token (invites and sessions): never the token itself."""
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def invite_json(row):
    """The invite as the dashboard may see it after creation: no token, no link (it is shown once)."""
    return {
        "id": row["id"],
        "created_by": row["created_by_username"] if "created_by_username" in row.keys() else row["created_by"],
        "created_by_id": row["created_by"],
        "note": row["note"],
        "created_at": row["created_at"],
        "expires_at": row["expires_at"],
        "max_uses": row["max_uses"],
        "uses": row["uses"],
        "revoked": row["revoked_at"] is not None,
        "last_email_to": row["last_email_to"],
    }


def invite_state(row, now=None):
    """'valid' | 'revoked' | 'expired' | 'exhausted'."""
    now = now or now_iso()
    if row["revoked_at"] is not None:
        return "revoked"
    if row["expires_at"] is not None and row["expires_at"] <= now:
        return "expired"
    if row["max_uses"] is not None and row["uses"] >= row["max_uses"]:
        return "exhausted"
    return "valid"


def get_invite_by_token(conn, token):
    return conn.execute(f"{INVITE_SELECT} WHERE i.token_hash = ?", (token_hash(token),)).fetchone()


def get_invite(conn, invite_id):
    return conn.execute(f"{INVITE_SELECT} WHERE i.id = ?", (invite_id,)).fetchone()


def create_invite(conn, created_by, max_uses=None, expires_at=None, note=None):
    """Returns (row, token): the token exists only in this return value from here on."""
    token = secrets.token_urlsafe(16)
    cur = conn.execute("INSERT INTO invites (token_hash, created_by, note, expires_at, max_uses) "
                       "VALUES (?, ?, ?, ?, ?)", (token_hash(token), created_by, note, expires_at, max_uses))
    return get_invite(conn, cur.lastrowid), token


def list_invites(conn, created_by=None):
    if created_by is None:
        return conn.execute(f"{INVITE_SELECT} ORDER BY i.id DESC").fetchall()
    return conn.execute(f"{INVITE_SELECT} WHERE i.created_by = ? ORDER BY i.id DESC", (created_by,)).fetchall()
