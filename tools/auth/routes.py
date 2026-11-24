"""HTTP surface of the sign-in gate.

Public (no cookie):   /auth/api/config, /auth/api/invites/<token>, /auth/api/register/*, /auth/api/login/*, logout
Session required:     /api/me, /api/me/passkeys/*, /api/settings, /api/users*, /api/invites*, /api/players

serve.py owns /api/saves* and /api/rom and calls gate() before serving anything else; when this module returns False
from handle(), the request was not ours and falls through to the existing static-file code.

Every response is JSON with Cache-Control: no-store; errors are {"error": "<code>", "message": "<sentence>"}.
Non-GET requests need Content-Type: application/json (when they carry a body), a same-origin Origin header (or
Sec-Fetch-Site: same-origin), and no CORS headers are ever emitted: that is the CSRF story together with
SameSite=Lax on the cookie.
"""
import json
import re
import secrets
import smtplib
import sqlite3
from urllib.parse import quote, urlsplit

from . import db, mail, sessions
from . import webauthn as wa

MAX_JSON_BYTES = 64 * 1024
USERNAME_RE = re.compile(r"^[A-Za-z0-9_.-]{3,32}$")
PUBLIC_PREFIXES = ("/auth/", "/invite/")
PUBLIC_PATHS = {"/favicon.ico"}
OWN_API_PATHS = {"/api/rom", "/api/saves"}
OWN_API_PREFIXES = ("/api/saves/",)


class HttpError(Exception):
    def __init__(self, status, code, message=""):
        super().__init__(code)
        self.status = status
        self.code = code
        self.message = message or code


# --- responses --------------------------------------------------------------------------------------------------

def _send(handler, status, body=b"", headers=()):
    handler.send_response(status)
    for name, value in headers:
        handler.send_header(name, value)
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    if handler.command != "HEAD" and body:
        handler.wfile.write(body)


def _json(handler, status, payload, headers=()):
    _send(handler, status, json.dumps(payload, separators=(",", ":")).encode("utf-8"),
          [("Content-Type", "application/json"), ("Cache-Control", "no-store"),
           ("X-Content-Type-Options", "nosniff"), *headers])


def _error(handler, status, code, message=""):
    _json(handler, status, {"error": code, "message": message or code})


# --- request helpers --------------------------------------------------------------------------------------------

def _body(handler):
    length = handler.headers.get("Content-Length")
    if not length:
        return {}
    try:
        size = int(length)
    except ValueError:
        raise HttpError(400, "request_invalid", "Content-Length must be an integer")
    if size < 0 or size > MAX_JSON_BYTES:
        raise HttpError(400, "request_invalid", "request body too large")
    if size == 0:
        return {}
    content_type = (handler.headers.get("Content-Type") or "").split(";")[0].strip().lower()
    if content_type != "application/json":
        raise HttpError(403, "cross_origin", "JSON requests only")
    try:
        payload = json.loads(handler.rfile.read(size) or b"{}")
    except (ValueError, UnicodeDecodeError):
        raise HttpError(400, "request_invalid", "body is not JSON")
    if not isinstance(payload, dict):
        raise HttpError(400, "request_invalid", "body must be a JSON object")
    return payload


def _same_origin(handler, config):
    origin = handler.headers.get("Origin")
    if origin:
        return config.is_origin(origin)
    return (handler.headers.get("Sec-Fetch-Site") or "").strip().lower() == "same-origin"


def _session_token(handler):
    return sessions.from_header(handler.headers.get("Cookie"))


def _user(handler, conn):
    return sessions.resolve(conn, _session_token(handler))


def _require_user(handler, conn):
    user = _user(handler, conn)
    if user is None:
        raise HttpError(401, "unauthorized", "sign in first")
    return user


def user_id(handler, db_path):
    """The signed-in user's id for serve.py's own APIs (tools/saves.py), or None."""
    with db.open_db(db_path) as conn:
        user = _user(handler, conn)
    return user["id"] if user is not None else None


def _require_owner(handler, conn):
    user = _require_user(handler, conn)
    if not user["is_owner"]:
        raise HttpError(403, "owner_only", "the owner does this")
    return user


def _username(value):
    name = (value or "").strip() if isinstance(value or "", str) else ""
    if not USERNAME_RE.match(name):
        raise HttpError(400, "username_invalid", "3-32 characters: letters, digits, dot, dash, underscore")
    return name


def _passkey_name(value):
    name = value if isinstance(value, str) else ""
    name = name.strip()
    if not name:
        return "Passkey"
    if len(name) > 40:
        raise HttpError(400, "name_invalid", "passkey names are at most 40 characters")
    return name


def _int_or_none(value, low, high):
    if value is None or value == "":
        return None
    if isinstance(value, bool) or not isinstance(value, int) or value < low or value > high:
        raise HttpError(400, "request_invalid", f"expected an integer between {low} and {high}")
    return value


def _credential_id(payload):
    credential = payload.get("credential")
    if not isinstance(credential, dict):
        raise HttpError(400, "credential_invalid", "credential is required")
    raw_id = credential.get("id") or credential.get("rawId")
    if not isinstance(raw_id, str) or not raw_id:
        raise HttpError(400, "credential_invalid", "credential id is required")
    return credential, raw_id.rstrip("=")


def _transports(payload):
    credential = payload.get("credential")
    response = credential.get("response") if isinstance(credential, dict) else None
    value = response.get("transports") if isinstance(response, dict) else None
    if isinstance(value, list) and all(isinstance(item, str) for item in value):
        return json.dumps(value)
    return None


def _user_with_counts(conn, user):
    payload = db.user_json(user)
    payload["disabled"] = bool(user["disabled"])
    payload["passkey_count"] = len(db.list_credentials(conn, user["id"]))
    return payload


_CONFIG = None


def configure(config):
    """Called once at startup by tools/auth/__init__.configure(); config is immutable, so reads are thread-safe."""
    global _CONFIG
    _CONFIG = config


# --- public: config / invite preflight --------------------------------------------------------------------------

def _config(handler, conn, match):
    _json(handler, 200, {
        "auth": True,
        "has_users": db.count_users(conn) > 0,
        "registration_open": db.registration_open(conn),
        "rp_name": _CONFIG.rp_name,
        "email_enabled": _CONFIG.smtp.configured,
    })


def _invite_preflight(handler, conn, match):
    invite = db.get_invite_by_token(conn, match["token"])
    if invite is None:
        raise HttpError(404, "invite_invalid", "that invite link does not exist")
    state = db.invite_state(invite)
    if state != "valid":
        raise HttpError(409, f"invite_{state}", f"that invite link is {state}")
    remaining = None if invite["max_uses"] is None else max(0, invite["max_uses"] - invite["uses"])
    _json(handler, 200, {"valid": True, "expires_at": invite["expires_at"], "remaining_uses": remaining})


# --- registration -----------------------------------------------------------------------------------------------

def _registration_allowed(conn, invite):
    """Bootstrap (no users yet) -> open registration -> a valid invite. Anything else is closed.

    register/options calls this to fail early; register/complete calls it again inside BEGIN IMMEDIATE with a freshly
    read invite, which is the check that actually decides."""
    if db.count_users(conn) == 0:
        return
    if invite is not None:
        state = db.invite_state(invite)
        if state != "valid":
            raise HttpError(409, f"invite_{state}", f"that invite link is {state}")
        return
    if not db.registration_open(conn):
        raise HttpError(403, "registration_closed", "registration is closed; ask for an invite link")


def _register_options(handler, conn, match):
    body = _body(handler)
    username = _username(body.get("username"))
    token = body.get("invite")
    invite = db.get_invite_by_token(conn, token) if isinstance(token, str) and token else None
    if invite is None and token:
        raise HttpError(404, "invite_invalid", "that invite link does not exist")
    _registration_allowed(conn, invite)
    if db.get_user_by_username(conn, username) is not None:
        raise HttpError(409, "username_taken", "that username is taken")
    handle = secrets.token_bytes(16)   # becomes the WebAuthn user.id and the users.handle of the new account
    challenge_b64, options = wa.registration_options(_CONFIG, handle, username)
    challenge_id = db.create_challenge(conn, challenge_b64, "register", username=username, handle=handle,
                                       invite_id=invite["id"] if invite is not None else None)
    _json(handler, 200, {"challenge_id": challenge_id, "publicKey": options})


def _register_complete(handler, conn, match):
    body = _body(handler)
    challenge_id = body.get("challenge_id")
    if not isinstance(challenge_id, str) or not challenge_id:
        raise HttpError(400, "request_invalid", "challenge_id is required")
    stored = db.take_challenge(conn, challenge_id, "register")
    if stored is None:
        raise HttpError(410, "challenge_expired", "start again: the sign-in challenge expired")
    username = stored["username"]
    credential, _ = _credential_id(body)
    name = _passkey_name(body.get("name"))
    try:
        verified = wa.verify_registration(_CONFIG, stored["challenge"], credential)
    except wa.VerificationFailed as err:
        raise HttpError(400, "credential_invalid", str(err))
    credential_id = wa.b64url(verified.credential_id)

    # Everything that can be raced is decided under the write lock, after the (slow) signature check: the invite may
    # have been spent or revoked since register/options, the username may have been taken, and the owner is whoever
    # wins the race to the first row. The conditional UPDATE is the hard guarantee for single-use links.
    conn.execute("BEGIN IMMEDIATE")
    try:
        invite = db.get_invite(conn, stored["invite_id"]) if stored["invite_id"] is not None else None
        _registration_allowed(conn, invite)
        if db.get_user_by_username(conn, username) is not None:
            raise HttpError(409, "username_taken", "that username is taken")
        if db.get_credential(conn, credential_id) is not None:
            raise HttpError(409, "credential_exists", "that passkey is already registered")
        if invite is not None:
            consumed = conn.execute(
                "UPDATE invites SET uses = uses + 1 WHERE id = ? AND revoked_at IS NULL "
                "AND (expires_at IS NULL OR expires_at > ?) AND (max_uses IS NULL OR uses < max_uses)",
                (invite["id"], db.now_iso()))
            if consumed.rowcount != 1:
                raise HttpError(409, "invite_exhausted", "that invite link has just been used")
        user = db.create_user(conn, username, stored["handle"])
        if user["is_owner"]:
            db.adopt_orphan_saves(conn, user["id"])
        db.create_credential(conn, user["id"], credential_id, bytes(verified.credential_public_key),
                             verified.sign_count, _transports(body), str(verified.aaguid),
                             verified.credential_backed_up, verified.credential_backed_up, name)
        conn.execute("COMMIT")
    except BaseException:
        conn.execute("ROLLBACK")
        raise
    token = sessions.issue(conn, user["id"], _CONFIG.session_days, handler.headers.get("User-Agent"))
    _json(handler, 200, {"user": _user_with_counts(conn, db.get_user(conn, user["id"]))},
          [("Set-Cookie", sessions.cookie_header(_CONFIG, token))])


# --- login / logout ---------------------------------------------------------------------------------------------

def _login_options(handler, conn, match):
    _body(handler)
    challenge_b64, options = wa.authentication_options(_CONFIG)
    challenge_id = db.create_challenge(conn, challenge_b64, "login")
    _json(handler, 200, {"challenge_id": challenge_id, "publicKey": options})


def _login_complete(handler, conn, match):
    body = _body(handler)
    challenge_id = body.get("challenge_id")
    if not isinstance(challenge_id, str) or not challenge_id:
        raise HttpError(400, "request_invalid", "challenge_id is required")
    stored = db.take_challenge(conn, challenge_id, "login")
    if stored is None:
        raise HttpError(410, "challenge_expired", "start again: the sign-in challenge expired")
    credential, credential_id = _credential_id(body)
    stored_credential = db.get_credential(conn, credential_id)
    if stored_credential is None:
        raise HttpError(401, "credential_unknown", "that passkey is not registered here")
    try:
        verified = wa.verify_authentication(_CONFIG, stored["challenge"], credential, stored_credential)
    except wa.VerificationFailed as err:
        raise HttpError(401, "credential_invalid", str(err))
    user = db.get_user(conn, stored_credential["user_id"])
    if user is None:
        raise HttpError(401, "credential_unknown", "that passkey is not registered here")
    if user["disabled"]:
        raise HttpError(403, "user_disabled", "this account is disabled")
    response = credential.get("response")
    handle = response.get("userHandle") if isinstance(response, dict) else None
    if isinstance(handle, str) and handle and handle.rstrip("=") != wa.b64url(bytes(user["handle"])):
        raise HttpError(401, "credential_invalid", "passkey belongs to another account")
    conn.execute("UPDATE credentials SET sign_count = ?, last_used_at = ? WHERE id = ?",
                 (verified.new_sign_count, db.now_iso(), credential_id))
    conn.execute("UPDATE users SET last_login_at = ? WHERE id = ?", (db.now_iso(), user["id"]))
    token = sessions.issue(conn, user["id"], _CONFIG.session_days, handler.headers.get("User-Agent"))
    _json(handler, 200, {"user": _user_with_counts(conn, db.get_user(conn, user["id"]))},
          [("Set-Cookie", sessions.cookie_header(_CONFIG, token))])


def _logout(handler, conn, match):
    sessions.revoke(conn, _session_token(handler))
    _send(handler, 204, b"", [("Set-Cookie", sessions.clear_cookie_header(_CONFIG)),
                              ("Cache-Control", "no-store")])


# --- /api/me ----------------------------------------------------------------------------------------------------

def _me(handler, conn, match):
    user = _require_user(handler, conn)
    _json(handler, 200, {
        "user": _user_with_counts(conn, user),
        "passkeys": [db.credential_json(row) for row in db.list_credentials(conn, user["id"])],
        "email_enabled": _CONFIG.smtp.configured,
    })


def _my_passkey_options(handler, conn, match):
    user = _require_user(handler, conn)
    _body(handler)
    existing_ids = [row["id"] for row in db.list_credentials(conn, user["id"])]
    challenge_b64, options = wa.registration_options(_CONFIG, bytes(user["handle"]), user["username"],
                                                     existing_ids)
    challenge_id = db.create_challenge(conn, challenge_b64, "add_passkey", user_id=user["id"])
    _json(handler, 200, {"challenge_id": challenge_id, "publicKey": options})


def _my_passkey_complete(handler, conn, match):
    user = _require_user(handler, conn)
    body = _body(handler)
    challenge_id = body.get("challenge_id")
    if not isinstance(challenge_id, str) or not challenge_id:
        raise HttpError(400, "request_invalid", "challenge_id is required")
    stored = db.take_challenge(conn, challenge_id, "add_passkey")
    if stored is None:
        raise HttpError(410, "challenge_expired", "start again: the sign-in challenge expired")
    if stored["user_id"] != user["id"]:
        raise HttpError(403, "unauthorized", "that challenge belongs to another session")
    credential, _ = _credential_id(body)
    name = _passkey_name(body.get("name"))
    try:
        verified = wa.verify_registration(_CONFIG, stored["challenge"], credential)
    except wa.VerificationFailed as err:
        raise HttpError(400, "credential_invalid", str(err))
    credential_id = wa.b64url(verified.credential_id)
    if db.get_credential(conn, credential_id) is not None:
        raise HttpError(409, "credential_exists", "that passkey is already registered")
    db.create_credential(conn, user["id"], credential_id, bytes(verified.credential_public_key),
                         verified.sign_count, _transports(body), str(verified.aaguid),
                         verified.credential_backed_up, verified.credential_backed_up, name)
    _json(handler, 201, {"passkey": db.credential_json(db.get_credential(conn, credential_id))})


def _rename_passkey(handler, conn, match):
    user = _require_user(handler, conn)
    row = db.get_credential(conn, match["id"])
    if row is None or row["user_id"] != user["id"]:
        raise HttpError(404, "not_found", "no such passkey")
    body = _body(handler)
    name = body.get("name")
    if not isinstance(name, str) or not name.strip() or len(name.strip()) > 40:
        raise HttpError(400, "name_invalid", "name must be 1-40 characters")
    name = name.strip()
    conn.execute("UPDATE credentials SET name = ? WHERE id = ?", (name, row["id"]))
    _json(handler, 200, {"passkey": db.credential_json(db.get_credential(conn, row["id"]))})


def _delete_passkey(handler, conn, match):
    user = _require_user(handler, conn)
    row = db.get_credential(conn, match["id"])
    if row is None or row["user_id"] != user["id"]:
        raise HttpError(404, "not_found", "no such passkey")
    if len(db.list_credentials(conn, user["id"])) <= 1:
        raise HttpError(400, "last_passkey", "add another passkey before removing this one")
    conn.execute("DELETE FROM credentials WHERE id = ?", (row["id"],))
    _send(handler, 204, b"", [("Cache-Control", "no-store")])


# --- settings / users (owner) -----------------------------------------------------------------------------------

def _get_settings(handler, conn, match):
    _require_user(handler, conn)
    _json(handler, 200, {"registration_open": db.registration_open(conn)})


def _patch_settings(handler, conn, match):
    _require_owner(handler, conn)
    body = _body(handler)
    if "registration_open" not in body or not isinstance(body["registration_open"], bool):
        raise HttpError(400, "request_invalid", "registration_open must be true or false")
    db.set_setting(conn, db.REGISTRATION_OPEN, "1" if body["registration_open"] else "0")
    _json(handler, 200, {"registration_open": db.registration_open(conn)})


def _list_users(handler, conn, match):
    _require_owner(handler, conn)
    _json(handler, 200, {"users": [dict(_user_with_counts(conn, row), passkey_count=row["passkey_count"])
                                   for row in db.list_users(conn)]})


def _list_players(handler, conn, match):
    _require_user(handler, conn)
    _json(handler, 200, {"players": db.list_players(conn)})


def _patch_user(handler, conn, match):
    owner = _require_owner(handler, conn)
    target = db.get_user(conn, int(match["id"]))
    if target is None:
        raise HttpError(404, "not_found", "no such user")
    if target["id"] == owner["id"]:
        raise HttpError(400, "cannot_modify_self", "you cannot change your own account here")
    if target["is_owner"]:
        raise HttpError(400, "cannot_modify_owner", "the owner account cannot be changed")
    body = _body(handler)
    if "disabled" not in body or not isinstance(body["disabled"], bool):
        raise HttpError(400, "request_invalid", "disabled must be true or false")
    conn.execute("UPDATE users SET disabled = ? WHERE id = ?", (int(body["disabled"]), target["id"]))
    if body["disabled"]:
        sessions.revoke_user(conn, target["id"])
    _json(handler, 200, {"user": _user_with_counts(conn, db.get_user(conn, target["id"]))})


def _delete_user(handler, conn, match):
    owner = _require_owner(handler, conn)
    target = db.get_user(conn, int(match["id"]))
    if target is None:
        raise HttpError(404, "not_found", "no such user")
    if target["id"] == owner["id"]:
        raise HttpError(400, "cannot_delete_self", "you cannot delete your own account")
    if target["is_owner"]:
        raise HttpError(400, "cannot_delete_owner", "the owner account cannot be deleted")
    sessions.revoke_user(conn, target["id"])
    conn.execute("DELETE FROM users WHERE id = ?", (target["id"],))
    _send(handler, 204, b"", [("Cache-Control", "no-store")])


# --- invites ----------------------------------------------------------------------------------------------------

def _list_invites(handler, conn, match):
    user = _require_user(handler, conn)
    rows = db.list_invites(conn, None if user["is_owner"] else user["id"])
    _json(handler, 200, {"invites": [db.invite_json(row) for row in rows]})


def _create_invite(handler, conn, match):
    user = _require_user(handler, conn)
    body = _body(handler)
    max_uses = _int_or_none(body.get("max_uses"), 1, 1000)
    days = _int_or_none(body.get("expires_in_days"), 1, 365)
    note = body.get("note")
    if note is not None and (not isinstance(note, str) or len(note) > 200):
        raise HttpError(400, "request_invalid", "note is at most 200 characters")
    to = body.get("email")
    if to is not None:
        try:
            to = mail.check_address(to)
        except mail.InvalidAddress:
            raise HttpError(400, "address_invalid", "a single recipient address is required")
        if not _CONFIG.smtp.configured:
            raise HttpError(503, "smtp_not_configured", "this server has no SMTP account configured")
    expires_at = db.iso_from_now(days=days) if days is not None else None
    invite, token = db.create_invite(conn, user["id"], max_uses=max_uses, expires_at=expires_at,
                                     note=(note or None))
    url = _CONFIG.invite_url(token)
    sent_to = None
    if to is not None:
        try:
            mail.send_invite(_CONFIG, to, url, invite["max_uses"], invite["expires_at"])
        except (smtplib.SMTPException, OSError) as err:
            # never leave behind an invite whose link nobody has seen
            conn.execute("UPDATE invites SET revoked_at = ? WHERE id = ?", (db.now_iso(), invite["id"]))
            raise HttpError(502, "smtp_failed", f"the mail server refused it: {err}")
        conn.execute("UPDATE invites SET last_email_to = ? WHERE id = ?", (to, invite["id"]))
        invite = db.get_invite(conn, invite["id"])
        sent_to = to
    _json(handler, 201, {"invite": db.invite_json(invite), "token": token, "url": url,
                         "email_sent_to": sent_to})


def _owned_invite(handler, conn, match, owner_ok=True):
    user = _require_user(handler, conn)
    invite = db.get_invite(conn, int(match["id"]))
    if invite is None:
        raise HttpError(404, "not_found", "no such invite")
    if invite["created_by"] != user["id"] and not (owner_ok and user["is_owner"]):
        raise HttpError(404, "not_found", "no such invite")
    return user, invite


def _revoke_invite(handler, conn, match):
    _, invite = _owned_invite(handler, conn, match)
    if invite["revoked_at"] is None:
        conn.execute("UPDATE invites SET revoked_at = ? WHERE id = ?", (db.now_iso(), invite["id"]))
    _send(handler, 204, b"", [("Cache-Control", "no-store")])


# --- routing ----------------------------------------------------------------------------------------------------

ROUTES = [
    ("GET", r"^/auth/api/config$", _config),
    ("GET", r"^/auth/api/invites/(?P<token>[A-Za-z0-9_-]{8,64})$", _invite_preflight),
    ("POST", r"^/auth/api/register/options$", _register_options),
    ("POST", r"^/auth/api/register/complete$", _register_complete),
    ("POST", r"^/auth/api/login/options$", _login_options),
    ("POST", r"^/auth/api/login/complete$", _login_complete),
    ("POST", r"^/auth/api/logout$", _logout),
    ("GET", r"^/api/me$", _me),
    ("POST", r"^/api/me/passkeys/options$", _my_passkey_options),
    ("POST", r"^/api/me/passkeys/complete$", _my_passkey_complete),
    ("PATCH", r"^/api/me/passkeys/(?P<id>[A-Za-z0-9_-]+)$", _rename_passkey),
    ("DELETE", r"^/api/me/passkeys/(?P<id>[A-Za-z0-9_-]+)$", _delete_passkey),
    ("GET", r"^/api/settings$", _get_settings),
    ("PATCH", r"^/api/settings$", _patch_settings),
    ("GET", r"^/api/users$", _list_users),
    ("PATCH", r"^/api/users/(?P<id>\d+)$", _patch_user),
    ("DELETE", r"^/api/users/(?P<id>\d+)$", _delete_user),
    ("GET", r"^/api/players$", _list_players),
    ("GET", r"^/api/invites$", _list_invites),
    ("POST", r"^/api/invites$", _create_invite),
    ("DELETE", r"^/api/invites/(?P<id>\d+)$", _revoke_invite),
]
_COMPILED = [(method, re.compile(pattern), func) for method, pattern, func in ROUTES]


def is_public(path):
    return path in PUBLIC_PATHS or path.startswith(PUBLIC_PREFIXES)


def rewrite(path):
    """URL -> the file to serve, when the gate is on: the dashboard at "/", the game at /play, the splash for
    invite links. None means "the normal static file"."""
    if path == "/":
        return "/dashboard/index.html"
    if path == "/auth":
        return "/auth/index.html"
    if path in ("/play", "/play/"):
        return "/index.html"
    if path.startswith("/invite/"):
        return "/auth/index.html"
    return None


def _dispatch(conn, handler, path, method):
    for route_method, pattern, func in _COMPILED:
        if route_method != method:
            continue
        match = pattern.match(path)
        if match is not None:
            func(handler, conn, match.groupdict())
            return True
    return False


def handle(handler, db_path):
    """True when this module answered the request (or answered with an error)."""
    path = urlsplit(handler.path).path
    if not (path.startswith("/auth/api/") or path.startswith("/api/")):
        return False
    if path in OWN_API_PATHS or path.startswith(OWN_API_PREFIXES):
        return False
    method = handler.command.upper()
    try:
        if method not in ("GET", "HEAD") and not _same_origin(handler, _CONFIG):
            raise HttpError(403, "cross_origin", "cross-origin requests are not accepted")
        with db.open_db(db_path) as conn:
            if not _dispatch(conn, handler, path, method):
                raise HttpError(404, "not_found", "no such endpoint")
        return True
    except HttpError as err:
        _error(handler, err.status, err.code, err.message)
        return True
    except sqlite3.Error:
        _error(handler, 500, "storage_error", "the database is unhappy")
        return True


def gate(handler, db_path):
    """False when the request was denied (a redirect or a 401 was already sent)."""
    path = urlsplit(handler.path).path
    if is_public(path):
        return True
    with db.open_db(db_path) as conn:
        if _user(handler, conn) is not None:
            return True
    accept = handler.headers.get("Accept") or ""
    if handler.command in ("GET", "HEAD") and "text/html" in accept and not path.startswith("/api/"):
        _send(handler, 302, b"", [("Location", "/auth/?next=" + quote(handler.path, safe="")),
                                  ("Cache-Control", "no-store")])
        return False
    _error(handler, 401, "unauthorized", "sign in first")
    return False
