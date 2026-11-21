"""Optional passkey sign-in for tools/serve.py.

`WEBBY_AUTH=1` turns the game's static server into an invite-only one: a splash page with a single sign-in button,
WebAuthn passkeys as the only credential, a dashboard after login, owner-only user management and invite links.
With the variable unset (the default) nothing in this package is reached and every URL behaves exactly as before.

tools/serve.py uses:

    configure(path, environ=None) -> Config   # read the environment, create the auth tables in the saves DB
    enabled() -> bool
    handle(handler) -> bool                   # answered /auth/api/* or /api/{me,users,invites,settings}
    gate(handler) -> bool                     # False = denied, a redirect or 401 was already sent
    rewrite(path) -> str | None               # "/" -> dashboard, "/play" -> the game, "/invite/<t>" -> splash
"""
from pathlib import Path

from . import config as _config_module
from . import db, routes

_config = None
_db_path = None


def configure(path, environ=None):
    global _config, _db_path
    _config = _config_module.from_env(environ)
    _db_path = Path(path)
    if _config.enabled:
        db.init(_db_path, _config.registration_open)
    routes.configure(_config)
    return _config


def reset():
    """Forget the configuration again (tests; enabled() goes back to False)."""
    global _config, _db_path
    _config = None
    _db_path = None
    routes.configure(None)


def settings():
    """The Config this process was configured with (None before configure())."""
    return _config


def enabled():
    return bool(_config is not None and _config.enabled)


def handle(handler):
    return enabled() and routes.handle(handler, _db_path)


def gate(handler):
    return not enabled() or routes.gate(handler, _db_path)


def rewrite(path):
    return routes.rewrite(path) if enabled() else None
