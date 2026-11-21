"""Settings of the optional sign-in gate, all from the environment (see README "Optional sign-in").

One place decides the WebAuthn relying party, the canonical origin used for invite links and origin checks, the
session lifetime and the SMTP account. Everything else in tools/auth/ takes a Config.
"""
import os
from dataclasses import dataclass
from urllib.parse import urlsplit

DEFAULT_EXTERNAL_URL = "http://localhost:8080"
DEFAULT_RP_NAME = "Webby"
DEFAULT_SESSION_DAYS = 30
DEFAULT_SMTP_PORT = 587
TLS_MODES = ("starttls", "ssl", "none")
TRUE = {"1", "true", "yes", "on"}


def _bool(value, default=False):
    if value is None or value == "":
        return default
    return value.strip().lower() in TRUE


def _int(value, default):
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return default


def _origin_and_rp_id(external_url):
    """WEBBY_KONG_EXTERNAL_URL -> (origin, rp_id). The origin is used verbatim (scheme://host[:port]) and is the only
    origin the API accepts; the host without its port is the WebAuthn RP ID, which the spec requires to be a
    registrable domain suffix of that origin's host."""
    raw = (external_url or "").strip() or DEFAULT_EXTERNAL_URL
    if "://" not in raw:
        raw = "http://" + raw
    parts = urlsplit(raw)
    scheme = (parts.scheme or "http").lower()
    host = parts.hostname or "localhost"
    port = parts.port
    netloc = f"[{host}]" if ":" in host else host
    if port is not None:
        netloc = f"{netloc}:{port}"
    return f"{scheme}://{netloc}", host


@dataclass(frozen=True)
class SmtpConfig:
    host: str | None
    port: int
    username: str | None
    password: str | None
    sender: str | None
    tls: str
    timeout: float

    @property
    def configured(self):
        return bool(self.host and self.sender)


@dataclass(frozen=True)
class Config:
    enabled: bool
    origin: str
    rp_id: str
    rp_name: str
    session_days: int
    registration_open: bool
    secure_cookies: bool
    smtp: SmtpConfig

    @property
    def session_max_age(self):
        return self.session_days * 86400

    def invite_url(self, token):
        return f"{self.origin}/invite/{token}"

    def is_origin(self, value):
        return bool(value) and value.rstrip("/") == self.origin


def from_env(environ=None):
    env = os.environ if environ is None else environ
    origin, rp_id = _origin_and_rp_id(env.get("WEBBY_KONG_EXTERNAL_URL"))
    scheme = origin.split("://", 1)[0]
    username = (env.get("WEBBY_KONG_SMTP_USERNAME") or "").strip() or None
    tls = (env.get("WEBBY_KONG_SMTP_TLS") or "starttls").strip().lower()
    return Config(
        enabled=_bool(env.get("WEBBY_KONG_AUTH")),
        origin=origin,
        rp_id=rp_id,
        rp_name=(env.get("WEBBY_KONG_RP_NAME") or "").strip() or DEFAULT_RP_NAME,
        session_days=max(1, _int(env.get("WEBBY_KONG_SESSION_DAYS"), DEFAULT_SESSION_DAYS)),
        registration_open=_bool(env.get("WEBBY_KONG_REGISTRATION_OPEN"), default=True),
        secure_cookies=scheme == "https",
        smtp=SmtpConfig(
            host=(env.get("WEBBY_KONG_SMTP_HOST") or "").strip() or None,
            port=_int(env.get("WEBBY_KONG_SMTP_PORT"), DEFAULT_SMTP_PORT),
            username=username,
            password=env.get("WEBBY_KONG_SMTP_PASSWORD") or None,
            sender=(env.get("WEBBY_KONG_SMTP_FROM") or "").strip() or username,
            tls=tls if tls in TLS_MODES else "starttls",
            timeout=float(_int(env.get("WEBBY_KONG_SMTP_TIMEOUT"), 10)),
        ),
    )
