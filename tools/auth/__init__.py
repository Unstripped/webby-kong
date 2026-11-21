"""Optional passkey sign-in for tools/serve.py.

`WEBBY_KONG_AUTH=1` turns the game's static server into an invite-only one: a splash page with a single sign-in button,
WebAuthn passkeys as the only credential, a dashboard after login, owner-only user management and invite links.
With the variable unset (the default) nothing in this package is reached, every URL behaves exactly as before and
py_webauthn is never imported (so a bare `python tools/serve.py` needs no extra packages).

tools/serve.py uses:

    configure(path, environ=None, config=None) -> Config   # read (or take) the settings, create the auth tables
    enabled() -> bool
    handle(handler) -> bool                   # answered /auth/api/* or /api/{me,users,invites,settings}
    gate(handler) -> bool                     # False = denied, a redirect or 401 was already sent
    rewrite(path) -> str | None               # "/" -> dashboard, "/play" -> the game, "/invite/<t>" -> splash
"""
from pathlib import Path

from . import config as _config_module
from . import db

_config = None
_db_path = None
_routes = None

MISSING_DEPENDENCY = ("WEBBY_KONG_AUTH needs the sign-in requirements (py_webauthn): run "
                      "`pip install -r tools/requirements-server.txt` (or use the Docker image).")


def configure(path, environ=None, config=None):
    """Read the sign-in settings (`config`, already read by the caller, or `environ`) and open the auth tables."""
    global _config, _db_path, _routes
    _config = config if config is not None else _config_module.from_env(environ)
    _db_path = Path(path)
    _routes = None
    if not _config.enabled:
        return _config
    try:
        from . import routes  # noqa: PLC0415 - imports py_webauthn, which only an enabled gate needs
    except ImportError as err:
        raise RuntimeError(MISSING_DEPENDENCY) from err
    db.init(_db_path, _config.registration_open)
    routes.configure(_config)
    _routes = routes
    return _config


def reset():
    """Forget the configuration again (tests; enabled() goes back to False)."""
    global _config, _db_path, _routes
    _config = None
    _db_path = None
    _routes = None


def settings():
    """The Config this process was configured with (None before configure())."""
    return _config


def enabled():
    return bool(_config is not None and _config.enabled)


def handle(handler):
    return _routes is not None and _routes.handle(handler, _db_path)


def gate(handler):
    return _routes is None or _routes.gate(handler, _db_path)


def rewrite(path):
    return _routes.rewrite(path) if _routes is not None else None
