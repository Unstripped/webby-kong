"""tools/auth/: the optional passkey gate in front of tools/serve.py.

The WebAuthn answers are monkeypatched (a real browser is needed to produce a signed assertion), so what is under
test is the gate, the session handling, the authorisation rules around registration/invites/users, and the CSRF
checks. Option *generation* is still real py_webauthn output.
"""
import contextlib
import functools
import hashlib
import http.client
import json
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import threading
import time
import unittest
import unittest.mock
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import auth  # noqa: E402
import saves  # noqa: E402
import serve  # noqa: E402

ORIGIN = "http://localhost:8080"


class AuthServer:
    """A private copy of the page server, configured through tools/auth/ the way tools/serve.py does it."""

    def __init__(self, environ):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "index.html").write_text("<!doctype html><title>game</title>")
        (self.dir / "dashboard").mkdir()
        (self.dir / "dashboard" / "index.html").write_text("<!doctype html><title>dashboard</title>")
        (self.dir / "auth").mkdir()
        (self.dir / "auth" / "index.html").write_text("<!doctype html><title>Webby</title>")
        self.db_path = self.dir / "saves.sqlite3"
        saves.init(self.db_path)
        self.config = auth.configure(self.db_path, environ)

        class Quiet(serve.Handler):
            db_path = self.db_path

            def log_message(self, *args):
                pass

        self.server = serve.Server(("::1", 0), functools.partial(Quiet, directory=str(self.dir)))
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()
        auth.reset()

    def request(self, method, path, body=None, cookie=None, headers=None):
        conn = http.client.HTTPConnection("::1", self.port, timeout=8)
        names = {"Origin": ORIGIN}
        if body is not None:
            names["Content-Type"] = "application/json"
        if cookie:
            names["Cookie"] = f"webby_session={cookie}"
        names.update(headers or {})
        data = None if body is None else json.dumps(body).encode()
        conn.request(method, path, body=data, headers=names)
        resp = conn.getresponse()
        payload = resp.read()
        conn.close()
        return resp, payload

    def db(self):
        conn = auth.db.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        return conn


def fake_verified_registration(credential_id=b"credential-1", sign_count=0):
    return SimpleNamespace(credential_id=credential_id, credential_public_key=b"\x01\x02\x03",
                           sign_count=sign_count, aaguid="00000000-0000-0000-0000-000000000000",
                           credential_backed_up=False)


class AuthEnabledTest(unittest.TestCase):
    def setUp(self):
        self.app = AuthServer({"WEBBY_KONG_AUTH": "1", "WEBBY_KONG_EXTERNAL_URL": ORIGIN})
        self.addCleanup(self.app.close)
        self.addCleanup(auth.reset)
        self.registrations = []

        def verify_registration(config, challenge_b64, credential):
            self.registrations.append(credential)
            return fake_verified_registration(credential["id"].encode())

        def verify_authentication(config, challenge_b64, credential, stored):
            return SimpleNamespace(credential_id=credential["id"].encode(), new_sign_count=1,
                                   credential_backed_up=False)

        patches = [
            unittest.mock.patch.object(auth.webauthn, "verify_registration", verify_registration),
            unittest.mock.patch.object(auth.webauthn, "verify_authentication", verify_authentication),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    # --- helpers ------------------------------------------------------------------------------------------------

    def get(self, path, cookie=None, accept=None):
        return self.app.request("GET", path, cookie=cookie, headers={"Accept": accept} if accept else None)

    def post(self, path, body, cookie=None, **kw):
        return self.app.request("POST", path, body, cookie=cookie, **kw)

    def patch(self, path, body, cookie=None, **kw):
        return self.app.request("PATCH", path, body, cookie=cookie, **kw)

    def delete(self, path, cookie=None, **kw):
        return self.app.request("DELETE", path, cookie=cookie, **kw)

    @staticmethod
    def cookie_of(resp):
        value = resp.getheader("Set-Cookie") or ""
        return value.split(";", 1)[0].split("=", 1)[1] if value else None

    def json_of(self, payload):
        return json.loads(payload)

    def register(self, username, invite=None):
        body = {"username": username} if invite is None else {"username": username, "invite": invite}
        resp, payload = self.post("/auth/api/register/options", body)
        assert resp.status == 200, (resp.status, payload)
        options = self.json_of(payload)
        resp, payload = self.post("/auth/api/register/complete", {
            "challenge_id": options["challenge_id"],
            "credential": {"id": f"cred-{username}", "rawId": f"cred-{username}", "type": "public-key",
                           "response": {"clientDataJSON": "e30", "attestationObject": "e30"}},
        })
        assert resp.status == 200, (resp.status, payload)
        return self.cookie_of(resp), self.json_of(payload)["user"]

    def create_invite(self, cookie, **body):
        """POST /api/invites -> (invite, token, url). The token and url exist only in this one response."""
        resp, payload = self.post("/api/invites", body, cookie=cookie)
        self.assertEqual(resp.status, 201, payload)
        data = self.json_of(payload)
        return data["invite"], data["token"], data["url"]

    def register_options(self, username, invite=None):
        body = {"username": username} if invite is None else {"username": username, "invite": invite}
        resp, payload = self.post("/auth/api/register/options", body)
        self.assertEqual(resp.status, 200, payload)
        return self.json_of(payload)

    def complete_registration(self, options, username):
        return self.post("/auth/api/register/complete", {
            "challenge_id": options["challenge_id"],
            "credential": {"id": f"cred-{username}", "type": "public-key", "response": {}},
        })

    def login(self, credential_id):
        resp, payload = self.post("/auth/api/login/options", {})
        assert resp.status == 200
        options = self.json_of(payload)
        resp, payload = self.post("/auth/api/login/complete", {
            "challenge_id": options["challenge_id"],
            "credential": {"id": credential_id, "rawId": credential_id, "type": "public-key",
                           "response": {"clientDataJSON": "e30", "authenticatorData": "e30",
                                        "signature": "e30"}},
        })
        return resp, payload

    def owner(self):
        return self.register("alice")

    def stored_credential(self, username):
        with contextlib.closing(self.app.db()) as conn:
            row = conn.execute("SELECT c.id FROM credentials c JOIN users u ON u.id = c.user_id "
                               "WHERE u.username = ?", (username,)).fetchone()
        return row["id"]

    def set_registration(self, cookie, open_):
        resp, payload = self.patch("/api/settings", {"registration_open": open_}, cookie=cookie)
        self.assertEqual(resp.status, 200, payload)

    # --- the gate -----------------------------------------------------------------------------------------------

    def test_anonymous_html_navigation_is_redirected_to_the_splash(self):
        resp, _ = self.get("/", accept="text/html")
        self.assertEqual(resp.status, 302)
        self.assertEqual(resp.getheader("Location"), "/auth/?next=%2F")
        resp, _ = self.get("/play?drawdist=2", accept="text/html")
        self.assertEqual(resp.status, 302)
        self.assertEqual(resp.getheader("Location"), "/auth/?next=%2Fplay%3Fdrawdist%3D2")

    def test_anonymous_api_and_assets_are_401_json(self):
        for path in ("/api/me", "/api/saves", "/api/saves/1/eeprom", "/api/players", "/api/rom", "/dk64.wasm"):
            resp, payload = self.get(path, accept="*/*")
            self.assertEqual(resp.status, 401, path)
            self.assertEqual(self.json_of(payload)["error"], "unauthorized")
        resp, _ = self.get("/favicon.ico")
        self.assertEqual(resp.status, 404)  # public, just missing

    def test_public_surface_needs_no_cookie(self):
        resp, payload = self.get("/auth/api/config")
        self.assertEqual(resp.status, 200)
        self.assertEqual(self.json_of(payload), {"auth": True, "has_users": False, "registration_open": True,
                                                 "rp_name": "Webby", "email_enabled": False})
        self.assertEqual(self.get("/auth/", accept="text/html")[0].status, 200)

    def test_sign_in_then_play_and_dashboard(self):
        cookie, user = self.owner()
        self.assertTrue(user["owner"])
        resp, payload = self.get("/", cookie=cookie, accept="text/html")
        self.assertEqual(resp.status, 200)
        self.assertIn(b"dashboard", payload)
        resp, payload = self.get("/play", cookie=cookie, accept="text/html")
        self.assertEqual(resp.status, 200)
        self.assertIn(b"game", payload)
        resp, payload = self.get("/play/", cookie=cookie, accept="text/html")
        self.assertEqual((resp.status, resp.getheader("Location")), (301, "/play"))
        me = self.json_of(self.get("/api/me", cookie=cookie)[1])
        self.assertEqual(me["user"]["username"], "alice")
        self.assertEqual(len(me["passkeys"]), 1)

    def test_cookie_is_httponly_lax_and_not_secure_over_http(self):
        resp, payload = self.post("/auth/api/register/options", {"username": "alice"})
        options = self.json_of(payload)
        resp, payload = self.post("/auth/api/register/complete", {
            "challenge_id": options["challenge_id"],
            "credential": {"id": "cred-alice", "type": "public-key", "response": {}},
        })
        cookie = resp.getheader("Set-Cookie")
        self.assertIn("HttpOnly", cookie)
        self.assertIn("SameSite=Lax", cookie)
        self.assertIn("Max-Age=2592000", cookie)
        self.assertNotIn("Secure", cookie)

    def test_secure_cookie_and_origin_follow_the_external_url(self):
        auth.configure(self.app.db_path, {"WEBBY_KONG_AUTH": "1",
                                          "WEBBY_KONG_EXTERNAL_URL": "https://kong.example.com"})
        resp, payload = self.post("/auth/api/register/options", {"username": "alice"})
        self.assertEqual(resp.status, 403)  # the http://localhost:8080 Origin no longer matches
        resp, payload = self.post("/auth/api/register/options", {"username": "alice"},
                                  headers={"Origin": "https://kong.example.com"})
        options = self.json_of(payload)
        resp, payload = self.post("/auth/api/register/complete", {
            "challenge_id": options["challenge_id"],
            "credential": {"id": "cred-alice", "type": "public-key", "response": {}},
        }, headers={"Origin": "https://kong.example.com"})
        self.assertIn("Secure", resp.getheader("Set-Cookie"))

    def test_csrf_rules(self):
        owner, _ = self.owner()
        resp, _ = self.post("/api/invites", {"max_uses": 1}, headers={"Origin": "https://evil.example"})
        self.assertEqual(resp.status, 403)
        self.assertEqual(self.json_of(_)["error"], "cross_origin")
        resp, _ = self.post("/api/invites", {"max_uses": 1}, headers={"Origin": ""})
        self.assertEqual(resp.status, 403)
        resp, _ = self.post("/api/invites", {"max_uses": 1}, headers={"Sec-Fetch-Site": "same-origin"})
        self.assertEqual(resp.status, 401)  # cookie is the only thing missing now
        resp, _ = self.post("/api/invites", {"max_uses": 1}, cookie=owner,
                            headers={"Origin": ORIGIN, "Content-Type": "text/plain"})
        self.assertEqual(resp.status, 403)

    def test_unknown_api_paths_are_404_json(self):
        cookie, _ = self.owner()
        for path, use_cookie in (("/api/nope", cookie), ("/auth/api/nope", None)):
            resp, payload = self.get(path, cookie=use_cookie)
            self.assertEqual(resp.status, 404, path)
            self.assertEqual(self.json_of(payload)["error"], "not_found", path)

    def test_logout_clears_the_session(self):
        cookie, _ = self.owner()
        self.assertEqual(self.get("/api/me", cookie=cookie)[0].status, 200)
        resp, _ = self.post("/auth/api/logout", {}, cookie=cookie)
        self.assertEqual(resp.status, 204)
        self.assertIn("Max-Age=0", resp.getheader("Set-Cookie"))
        self.assertEqual(self.get("/api/me", cookie=cookie)[0].status, 401)

    # --- registration -------------------------------------------------------------------------------------------

    def test_first_user_is_the_owner_and_handle_matches_webauthn_user_id(self):
        resp, payload = self.post("/auth/api/register/options", {"username": "alice"})
        options = self.json_of(payload)
        resp, payload = self.post("/auth/api/register/complete", {
            "challenge_id": options["challenge_id"],
            "credential": {"id": "cred-alice", "type": "public-key", "response": {}},
            "name": "Phone",
        })
        self.assertEqual(resp.status, 200, payload)
        user = self.json_of(payload)["user"]
        self.assertTrue(user["owner"])
        with contextlib.closing(self.app.db()) as conn:
            row = conn.execute("SELECT handle FROM users WHERE username = 'alice'").fetchone()
        self.assertEqual(auth.webauthn.b64url(bytes(row["handle"])), options["publicKey"]["user"]["id"])
        me = self.json_of(self.get("/api/me", cookie=self.cookie_of(resp))[1])
        self.assertEqual(me["passkeys"][0]["name"], "Phone")

    def test_username_validation_and_uniqueness(self):
        self.owner()
        for bad in ("", "ab", "a b c", "x" * 33, "<script>"):
            resp, payload = self.post("/auth/api/register/options", {"username": bad})
            self.assertEqual(resp.status, 400, bad)
            self.assertEqual(self.json_of(payload)["error"], "username_invalid")
        resp, payload = self.post("/auth/api/register/options", {"username": "ALICE"})
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (409, "username_taken"))

    def test_registration_closed_by_the_owner_until_an_invite_arrives(self):
        owner, _ = self.owner()
        self.set_registration(owner, False)
        resp, payload = self.post("/auth/api/register/options", {"username": "bob"})
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (403, "registration_closed"))

        invite, token, url = self.create_invite(owner, max_uses=1, note="bob")
        self.assertTrue(url.startswith(ORIGIN + "/invite/"))
        self.assertEqual(url, f"{ORIGIN}/invite/{token}")
        cursor, user = self.register("bob", invite=token)
        self.assertFalse(user["owner"])
        with contextlib.closing(self.app.db()) as conn:
            self.assertEqual(conn.execute("SELECT uses FROM invites WHERE id = ?", (invite["id"],))
                             .fetchone()["uses"], 1)
        # registration stays closed for everybody else
        self.assertEqual(self.post("/auth/api/register/options", {"username": "carol"})[0].status, 403)

    def test_single_use_invite_is_exhausted(self):
        owner, _ = self.owner()
        self.set_registration(owner, False)
        _, token, _ = self.create_invite(owner, max_uses=1)
        self.register("bob", invite=token)
        resp, payload = self.post("/auth/api/register/options", {"username": "carol", "invite": token})
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (409, "invite_exhausted"))
        self.assertEqual(self.get(f"/auth/api/invites/{token}")[0].status, 409)

    def test_single_use_invite_is_consumed_by_only_one_of_two_completions(self):
        """Two challenges prepared against the same single-use link: the second completion must be refused."""
        owner, _ = self.owner()
        self.set_registration(owner, False)
        _, token, _ = self.create_invite(owner, max_uses=1)
        bob = self.register_options("bob", invite=token)
        carol = self.register_options("carol", invite=token)
        resp, payload = self.complete_registration(bob, "bob")
        self.assertEqual(resp.status, 200, payload)
        resp, payload = self.complete_registration(carol, "carol")
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (409, "invite_exhausted"))
        with contextlib.closing(self.app.db()) as conn:
            self.assertEqual(conn.execute("SELECT uses FROM invites").fetchone()["uses"], 1)
            self.assertEqual(conn.execute("SELECT count(*) AS n FROM users").fetchone()["n"], 2)  # alice + bob

    def test_concurrent_completions_consume_a_single_use_invite_once(self):
        owner, _ = self.owner()
        self.set_registration(owner, False)
        _, token, _ = self.create_invite(owner, max_uses=1)
        options = [self.register_options("bob", invite=token), self.register_options("carol", invite=token)]
        results = []

        def finish(pair):
            username, opts = pair
            resp, payload = self.complete_registration(opts, username)
            results.append((username, resp.status, payload))

        threads = [threading.Thread(target=finish, args=(pair,))
                   for pair in zip(("bob", "carol"), options)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(30)
        self.assertEqual(sorted(status for _, status, _ in results), [200, 409], results)
        with contextlib.closing(self.app.db()) as conn:
            self.assertEqual(conn.execute("SELECT uses FROM invites").fetchone()["uses"], 1)
            names = {row["username"] for row in conn.execute("SELECT username FROM users")}
        self.assertEqual(len(names), 2)  # alice + exactly one of bob/carol

    def test_concurrent_bootstrap_makes_exactly_one_owner(self):
        """Two fresh-install registrations at once: both may pass the zero-users check, only one becomes owner."""
        options = [self.register_options("alice"), self.register_options("bob")]
        results = []

        def finish(pair):
            username, opts = pair
            resp, payload = self.complete_registration(opts, username)
            results.append(resp.status)

        threads = [threading.Thread(target=finish, args=(pair,))
                   for pair in zip(("alice", "bob"), options)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(30)
        self.assertEqual(sorted(results), [200, 200])  # open registration: both accounts exist
        with contextlib.closing(self.app.db()) as conn:
            self.assertEqual(conn.execute("SELECT count(*) AS n FROM users WHERE is_owner = 1")
                             .fetchone()["n"], 1)

    def test_reusable_invite_allows_several_sign_ups(self):
        owner, _ = self.owner()
        self.set_registration(owner, False)
        _, token, _ = self.create_invite(owner, max_uses=3)
        self.register("bob", invite=token)
        self.register("carol", invite=token)
        preflight = self.json_of(self.get(f"/auth/api/invites/{token}")[1])
        self.assertEqual(preflight["remaining_uses"], 1)

    def test_expired_and_revoked_invites_are_refused(self):
        owner, _ = self.owner()
        self.set_registration(owner, False)
        expired, token, _ = self.create_invite(owner, expires_in_days=1)
        with contextlib.closing(self.app.db()) as conn:
            conn.execute("UPDATE invites SET expires_at = '2000-01-01T00:00:00.000Z' WHERE id = ?",
                         (expired["id"],))
        resp, payload = self.post("/auth/api/register/options", {"username": "bob", "invite": token})
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (409, "invite_expired"))

        revoked, token, _ = self.create_invite(owner, max_uses=1)
        self.assertEqual(self.delete(f"/api/invites/{revoked['id']}", cookie=owner)[0].status, 204)
        resp, payload = self.post("/auth/api/register/options", {"username": "carol", "invite": token})
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (409, "invite_revoked"))

    def test_unknown_invite_and_spent_challenge(self):
        resp, payload = self.post("/auth/api/register/options", {"username": "bob", "invite": "nope-nope-nope"})
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (404, "invite_invalid"))
        self.owner()
        options = self.register_options("bob")
        self.complete_registration(options, "bob")
        resp, payload = self.complete_registration(options, "bob")
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (410, "challenge_expired"))

    # --- login --------------------------------------------------------------------------------------------------

    def test_login_with_a_passkey_and_disable_revokes_the_session(self):
        self.owner()
        cookie, user = self.register("bob")
        credential_id = self.stored_credential("bob")
        resp, payload = self.login(credential_id)
        self.assertEqual(resp.status, 200, payload)
        self.assertEqual(self.json_of(payload)["user"]["username"], "bob")
        bob_cookie = self.cookie_of(resp)
        self.assertEqual(self.get("/api/me", cookie=bob_cookie)[0].status, 200)

        resp, _ = self.login(self.stored_credential("alice"))
        owner_cookie = self.cookie_of(resp)
        resp, _ = self.patch(f"/api/users/{user['id']}", {"disabled": True}, cookie=owner_cookie)
        self.assertEqual(resp.status, 200)
        self.assertEqual(self.get("/api/me", cookie=bob_cookie)[0].status, 401)
        resp, payload = self.login(credential_id)
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (403, "user_disabled"))

    def test_login_with_an_unknown_passkey(self):
        self.owner()
        resp, payload = self.login("nope-nope")
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (401, "credential_unknown"))

    def test_disabled_user_sessions_expire_immediately(self):
        owner, _ = self.owner()
        cookie, user = self.register("bob")
        self.patch(f"/api/users/{user['id']}", {"disabled": True}, cookie=owner)
        self.assertEqual(self.get("/api/me", cookie=cookie)[0].status, 401)

    # --- multi-passkey ------------------------------------------------------------------------------------------

    def test_add_rename_and_remove_passkeys(self):
        cookie, _ = self.owner()
        resp, payload = self.post("/api/me/passkeys/options", {}, cookie=cookie)
        self.assertEqual(resp.status, 200)
        options = self.json_of(payload)
        self.assertEqual(len(options["publicKey"]["excludeCredentials"]), 1)
        resp, payload = self.post("/api/me/passkeys/complete", {
            "challenge_id": options["challenge_id"],
            "credential": {"id": "cred-second", "response": {}},
            "name": "Laptop",
        }, cookie=cookie)
        self.assertEqual(resp.status, 201, payload)
        second = self.json_of(payload)["passkey"]
        self.assertEqual(second["name"], "Laptop")

        current = self.json_of(self.get("/api/me", cookie=cookie)[1])["passkeys"]
        names = {row["name"] for row in current}
        self.assertEqual(names, {"Passkey", "Laptop"})
        resp, payload = self.patch(f"/api/me/passkeys/{second['id']}", {"name": "Desk"}, cookie=cookie)
        self.assertEqual(self.json_of(payload)["passkey"]["name"], "Desk")
        self.assertEqual(self.patch(f"/api/me/passkeys/{second['id']}", {"name": ""}, cookie=cookie)[0].status,
                         400)
        self.assertEqual(self.delete(f"/api/me/passkeys/{second['id']}", cookie=cookie)[0].status, 204)
        self.assertEqual(self.delete(f"/api/me/passkeys/{second['id']}", cookie=cookie)[0].status, 404)
        resp, payload = self.delete(f"/api/me/passkeys/{current[0]['id']}", cookie=cookie)
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (400, "last_passkey"))

    # --- owner-only ---------------------------------------------------------------------------------------------

    def test_owner_only_endpoints_are_forbidden_for_others(self):
        owner, _ = self.owner()
        cookie, _ = self.register("bob")
        for method, path in (("GET", "/api/users"), ("PATCH", "/api/settings")):
            resp, payload = self.app.request(method, path, {"registration_open": True} if method == "PATCH"
                                             else None, cookie=cookie)
            self.assertEqual((resp.status, self.json_of(payload)["error"]), (403, "owner_only"), path)
        self.assertEqual(self.get("/api/settings", cookie=cookie)[0].status, 200)
        self.assertEqual(len(self.json_of(self.get("/api/users", cookie=owner)[1])["users"]), 2)

    def test_owner_cannot_delete_or_disable_itself_and_nobody_deletes_the_owner(self):
        owner_cookie, owner = self.owner()
        resp, payload = self.patch(f"/api/users/{owner['id']}", {"disabled": True}, cookie=owner_cookie)
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (400, "cannot_modify_self"))
        resp, payload = self.delete(f"/api/users/{owner['id']}", cookie=owner_cookie)
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (400, "cannot_delete_self"))
        self.assertEqual(self.get("/api/me", cookie=owner_cookie)[0].status, 200)

    def test_owner_deletes_a_user_and_their_credentials_and_invites_go(self):
        owner_cookie, _ = self.owner()
        _, bob = self.register("bob")
        resp, _ = self.login(self.stored_credential("bob"))
        bob_cookie = self.cookie_of(resp)
        invite, _, _ = self.create_invite(bob_cookie, max_uses=1)
        self.assertEqual(self.delete(f"/api/users/{bob['id']}", cookie=owner_cookie)[0].status, 204)
        self.assertEqual(self.get("/api/me", cookie=bob_cookie)[0].status, 401)
        with contextlib.closing(self.app.db()) as conn:
            self.assertEqual(conn.execute("SELECT count(*) AS n FROM credentials c JOIN users u ON u.id = "
                                          "c.user_id WHERE u.username = 'bob'").fetchone()["n"], 0)
            self.assertIsNone(conn.execute("SELECT 1 FROM invites WHERE id = ?", (invite["id"],)).fetchone())

    # --- save games per user ------------------------------------------------------------------------------------

    def create_save(self, cookie, name=None):
        resp, payload = self.post("/api/saves", {} if name is None else {"name": name}, cookie=cookie)
        self.assertEqual(resp.status, 201, payload)
        return self.json_of(payload)["save"]

    def put_eeprom(self, cookie, save_id, summary=None):
        headers = {"Cookie": f"webby_session={cookie}", "Origin": ORIGIN, "X-Save-Mtime": "1000"}
        if summary is not None:
            headers["X-Save-Summary"] = json.dumps(summary)
        conn = http.client.HTTPConnection("::1", self.app.port, timeout=8)
        conn.request("PUT", f"/api/saves/{save_id}/eeprom", body=bytes(2048), headers=headers)
        resp = conn.getresponse()
        resp.read()
        conn.close()
        return resp

    def test_saves_belong_to_their_user(self):
        alice_cookie, _ = self.owner()
        bob_cookie, _ = self.register("bob")
        alices = self.create_save(alice_cookie, "Alice's")
        resp, payload = self.get("/api/saves", cookie=alice_cookie)
        self.assertEqual(self.json_of(payload), {"saves": [alices], "dashboard": True})
        for path in (f"/api/saves/{alices['id']}", f"/api/saves/{alices['id']}/eeprom"):
            resp, payload = self.get(path, cookie=bob_cookie)
            self.assertEqual((resp.status, self.json_of(payload)["error"]), (404, "not_found"), path)
        self.assertEqual(self.put_eeprom(bob_cookie, alices["id"]).status, 404)
        self.assertEqual(self.delete(f"/api/saves/{alices['id']}", cookie=bob_cookie)[0].status, 404)
        self.assertEqual(self.json_of(self.get("/api/saves", cookie=bob_cookie)[1])["saves"], [])

    def test_every_user_sees_every_player_and_their_progress(self):
        alice_cookie, alice = self.owner()
        bob_cookie, bob = self.register("bob")
        save = self.create_save(alice_cookie)
        summary = {"slot": 0, "percent": 12, "goldenBananas": 9, "keys": 1, "crowns": 0, "medals": 0,
                   "fairies": 0, "blueprints": 2, "kongs": 2, "playSeconds": 3600}
        self.assertEqual(self.put_eeprom(alice_cookie, save["id"], summary).status, 204)
        resp, payload = self.get("/api/players", cookie=bob_cookie)
        self.assertEqual(resp.status, 200)
        players = {p["username"]: p for p in self.json_of(payload)["players"]}
        self.assertEqual(set(players), {"alice", "bob"})
        self.assertTrue(players["alice"]["owner"])
        self.assertEqual([(s["id"], s["name"], s["summary"]) for s in players["alice"]["saves"]],
                         [(save["id"], "Save 1", summary)])
        self.assertEqual(players["bob"]["saves"], [])
        self.assertFalse(players["bob"]["disabled"])
        self.assertEqual(self.get("/api/players")[0].status, 401)

    def test_the_owner_adopts_saves_made_before_sign_in_existed(self):
        with contextlib.closing(sqlite3.connect(self.app.db_path)) as conn:
            conn.execute("INSERT INTO saves (user_id, name) VALUES (NULL, 'Save 1')")
            conn.commit()
        alice_cookie, _ = self.owner()
        self.assertEqual([s["name"] for s in self.json_of(self.get("/api/saves", cookie=alice_cookie)[1])["saves"]],
                         ["Save 1"])
        bob_cookie, _ = self.register("bob")
        self.assertEqual(self.json_of(self.get("/api/saves", cookie=bob_cookie)[1])["saves"], [])

    def test_deleting_a_user_deletes_their_saves_and_history(self):
        owner_cookie, _ = self.owner()
        bob_cookie, bob = self.register("bob")
        save = self.create_save(bob_cookie)
        self.assertEqual(self.put_eeprom(bob_cookie, save["id"]).status, 204)
        self.assertEqual(self.delete(f"/api/users/{bob['id']}", cookie=owner_cookie)[0].status, 204)
        with contextlib.closing(self.app.db()) as conn:
            self.assertIsNone(conn.execute("SELECT 1 FROM saves WHERE id = ?", (save["id"],)).fetchone())
            self.assertIsNone(conn.execute("SELECT 1 FROM eeprom WHERE save_id = ?", (save["id"],)).fetchone())

    # --- invite tokens at rest and one-shot email ---------------------------------------------------------------

    def test_invite_token_is_never_stored_in_plaintext(self):
        owner, _ = self.owner()
        _, token, url = self.create_invite(owner, max_uses=1)
        self.assertIn(token, url)
        with contextlib.closing(self.app.db()) as conn:
            row = conn.execute("SELECT * FROM invites").fetchone()
            values = " ".join(str(row[key]) for key in row.keys())
            stored = row["token_hash"]
        self.assertNotIn(token, values)
        self.assertEqual(stored, hashlib.sha256(token.encode()).hexdigest())
        for suffix in ("", "-wal", "-shm"):
            path = Path(str(self.app.db_path) + suffix)
            if path.exists():
                self.assertNotIn(token.encode(), path.read_bytes(), f"{path} holds the plaintext token")

    def test_invite_email_is_sent_at_creation_or_refused(self):
        owner, _ = self.owner()
        resp, payload = self.post("/api/invites", {"max_uses": 1, "email": "bob@example.com"}, cookie=owner)
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (503, "smtp_not_configured"))
        self.assertEqual(self.json_of(self.get("/api/invites", cookie=owner)[1])["invites"], [],
                         "a refused email must not leave an invite behind")
        resp, payload = self.post("/api/invites", {"max_uses": 1, "email": "not an address"}, cookie=owner)
        self.assertEqual((resp.status, self.json_of(payload)["error"]), (400, "address_invalid"))
        self.assertEqual(self.json_of(self.get("/api/me", cookie=owner)[1])["email_enabled"], False)
        # the old per-invite endpoint is gone: the link is only ever shown once
        invite, _, _ = self.create_invite(owner, max_uses=1)
        self.assertEqual(self.post(f"/api/invites/{invite['id']}/email", {"to": "bob@example.com"},
                                   cookie=owner)[0].status, 404)

    def test_invite_list_exposes_no_token_or_url(self):
        owner, _ = self.owner()
        _, token, _ = self.create_invite(owner, max_uses=2, note="team")
        listed = self.json_of(self.get("/api/invites", cookie=owner)[1])["invites"]
        self.assertEqual(len(listed), 1)
        self.assertNotIn("url", listed[0])
        self.assertNotIn("token", listed[0])
        self.assertEqual(listed[0]["note"], "team")
        self.assertEqual(listed[0]["created_by"], "alice")
        self.assertNotIn(token, json.dumps(listed))

    def test_only_the_creator_or_the_owner_sees_an_invite(self):
        owner, _ = self.owner()
        resp, _ = self.login(self.stored_credential("alice"))
        alice_cookie = self.cookie_of(resp)
        bob_cookie, _ = self.register("bob")
        own, _, _ = self.create_invite(bob_cookie, max_uses=1)
        self.assertEqual(len(self.json_of(self.get("/api/invites", cookie=bob_cookie)[1])["invites"]), 1)
        self.assertEqual(len(self.json_of(self.get("/api/invites", cookie=alice_cookie)[1])["invites"]), 1)
        carol_cookie, _ = self.register("carol")
        self.assertEqual(self.json_of(self.get("/api/invites", cookie=carol_cookie)[1])["invites"], [])
        self.assertEqual(self.delete(f"/api/invites/{own['id']}", cookie=carol_cookie)[0].status, 404)


class ServeScriptTest(unittest.TestCase):
    """`python tools/serve.py` itself: a bare interpreter must work with the gate off, and must say what to
    install when the gate is on and py_webauthn is missing (a real subprocess with `import webauthn` poisoned)."""

    BOOTSTRAP = ("import runpy, sys\n"
                 "sys.modules['webauthn'] = None\n"
                 "sys.path.insert(0, 'tools')  # what `python tools/serve.py` does for you\n"
                 "runpy.run_path('tools/serve.py', run_name='__main__')\n")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        (self.dir / "index.html").write_text("<!doctype html><title>game</title>")
        # the sign-in pages too: with the gate on, a missing one stops serve.py before it imports py_webauthn
        for page in ("auth/index.html", "dashboard/index.html"):
            (self.dir / page).parent.mkdir(parents=True, exist_ok=True)
            (self.dir / page).write_text("<!doctype html>")
        self.db_path = self.dir / "saves.sqlite3"

    def command(self):
        return [sys.executable, "-c", self.BOOTSTRAP, "--dir", str(self.dir), "--bind", "127.0.0.1",
                "--db", str(self.db_path)]

    def env(self, **overrides):
        base = dict(os.environ, WEBBY_KONG_AUTH="", WEBBY_KONG_EXTERNAL_URL="http://localhost:8080")
        base.update(overrides)
        return base

    @staticmethod
    def free_port():
        with contextlib.closing(socket.socket()) as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    def test_auth_off_serves_without_py_webauthn(self):
        port = self.free_port()
        proc = subprocess.Popen(self.command() + ["--port", str(port)], cwd=ROOT, env=self.env(),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (proc.terminate(), proc.communicate(timeout=10)))
        deadline = time.monotonic() + 20
        while True:
            try:
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                conn.request("GET", "/")
                resp = conn.getresponse()
                body = resp.read()
                conn.close()
                break
            except OSError:
                if time.monotonic() > deadline or proc.poll() is not None:
                    self.fail(f"serve.py did not start:\n{proc.communicate()[0].decode(errors='replace')}")
                time.sleep(0.1)
        self.assertEqual(resp.status, 200)
        self.assertIn(b"<title>game", body)
        self.assertIsNone(proc.poll(), "serve.py exited with the gate off")

    def test_auth_on_without_py_webauthn_fails_loudly(self):
        done = subprocess.run(self.command() + ["--port", str(self.free_port())], cwd=ROOT,
                              env=self.env(WEBBY_KONG_AUTH="1"), capture_output=True, timeout=60, text=True)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("requirements-server.txt", done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
