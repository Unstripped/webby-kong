"""Session cookies: opaque token in the browser, sha256 of it in SQLite."""
import hashlib
import secrets

from . import db

COOKIE_NAME = "webby_session"
_TOKEN_BYTES = 32


def new_token():
    return secrets.token_urlsafe(_TOKEN_BYTES)


def token_id(token):
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def issue(conn, user_id, days, user_agent=None):
    """Create a session row and return the token to put in the cookie (never stored as-is)."""
    token = new_token()
    conn.execute("INSERT INTO sessions (id, user_id, expires_at, last_seen_at, user_agent) VALUES (?, ?, ?, ?, ?)",
                 (token_id(token), user_id, db.iso_from_now(days=days), db.now_iso(), user_agent))
    return token


def resolve(conn, token):
    """Cookie token -> the user row, or None when the session is unknown, expired or the user is disabled."""
    if not token:
        return None
    sid = token_id(token)
    row = conn.execute(
        "SELECT s.id AS session_id, s.expires_at, u.* FROM sessions s JOIN users u ON u.id = s.user_id "
        "WHERE s.id = ?", (sid,)).fetchone()
    if row is None:
        return None
    if row["expires_at"] <= db.now_iso():
        conn.execute("DELETE FROM sessions WHERE id = ?", (sid,))
        return None
    if row["disabled"]:
        revoke_user(conn, row["id"])
        return None
    conn.execute("UPDATE sessions SET last_seen_at = ? WHERE id = ?", (db.now_iso(), sid))
    return row


def revoke(conn, token):
    if token:
        conn.execute("DELETE FROM sessions WHERE id = ?", (token_id(token),))


def revoke_user(conn, user_id):
    conn.execute("DELETE FROM sessions WHERE user_id = ?", (user_id,))


def from_header(value):
    """Cookie header -> this app's session token (or None)."""
    if not value:
        return None
    for part in value.split(";"):
        name, _, val = part.strip().partition("=")
        if name == COOKIE_NAME:
            return val.strip() or None
    return None


def cookie_header(config, token):
    parts = [f"{COOKIE_NAME}={token}", "Path=/", "HttpOnly", "SameSite=Lax", f"Max-Age={config.session_max_age}"]
    if config.secure_cookies:
        parts.append("Secure")
    return "; ".join(parts)


def clear_cookie_header(config):
    parts = [f"{COOKIE_NAME}=", "Path=/", "HttpOnly", "SameSite=Lax", "Max-Age=0"]
    if config.secure_cookies:
        parts.append("Secure")
    return "; ".join(parts)
