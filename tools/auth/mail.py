"""Invite email: one HTML template (dark, neon gradient header, one CTA, inline CSS, table layout) and its text
alternative. Templates are module strings so the Docker image only has to copy tools/auth/.

Nothing here mentions a game: the product is "Webby".
"""
import smtplib
import ssl
from email.message import EmailMessage
from email.utils import formataddr

SUBJECT = "You're invited to Webby"

_STYLE = ("margin:0;padding:0;background-color:#0a0f1a;"
          "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;")

HTML = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark">
<title>You're invited to Webby</title>
</head>
<body style="{style}">
<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0"
       style="width:100%;background-color:#0a0f1a;padding:32px 12px;">
<tr><td align="center">
  <table role="presentation" width="600" cellpadding="0" cellspacing="0" border="0"
         style="width:100%;max-width:600px;background-color:#111827;border:1px solid #1f2937;border-radius:18px;">
    <tr><td style="padding:0;border-radius:18px 18px 0 0;background-color:#7c3aed;
                   background-image:linear-gradient(135deg,#22d3ee 0%,#a855f7 48%,#f472b6 100%);">
      <table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0">
        <tr><td align="center" style="padding:34px 24px;font-size:26px;line-height:1.2;font-weight:700;
                                       letter-spacing:.16em;text-transform:uppercase;color:#0a0f1a;">
          Webby
        </td></tr>
      </table>
    </td></tr>
    <tr><td style="padding:38px 40px 0 40px;">
      <h1 style="margin:0 0 14px 0;font-size:26px;line-height:1.25;font-weight:700;color:#f9fafb;">
        You're invited
      </h1>
      <p style="margin:0 0 10px 0;font-size:16px;line-height:1.6;color:#cbd5e1;">
        Someone set up an account for you on Webby. Use the button below to pick a username and add a passkey.
      </p>
      <p style="margin:0;font-size:14px;line-height:1.6;color:#94a3b8;">{terms}</p>
    </td></tr>
    <tr><td align="center" style="padding:28px 40px 0 40px;">
      <table role="presentation" cellpadding="0" cellspacing="0" border="0">
        <tr><td align="center" style="border-radius:14px;background-color:#22d3ee;
                                       background-image:linear-gradient(135deg,#22d3ee,#a855f7);">
          <a href="{url}" style="display:inline-block;padding:16px 34px;border-radius:14px;
                                 font-size:16px;font-weight:700;line-height:1.2;color:#0a0f1a;text-decoration:none;">
            Create your account
          </a>
        </td></tr>
      </table>
    </td></tr>
    <tr><td style="padding:24px 40px 8px 40px;">
      <p style="margin:0 0 6px 0;font-size:12px;line-height:1.5;color:#64748b;">
        Button not working? Paste this link into your browser:
      </p>
      <p style="margin:0;font-size:12px;line-height:1.5;word-break:break-all;">
        <a href="{url}" style="color:#5eead4;text-decoration:none;">{url}</a>
      </p>
    </td></tr>
    <tr><td style="padding:0 40px 34px 40px;">
      <table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0">
        <tr><td style="border-top:1px solid #1f2937;padding-top:18px;font-size:12px;line-height:1.6;color:#64748b;">
          If you weren't expecting this, ignore it. The link only works for the sign-ins shown above and can be
          revoked at any time.
        </td></tr>
      </table>
    </td></tr>
  </table>
  <table role="presentation" width="600" cellpadding="0" cellspacing="0" border="0"
         style="width:100%;max-width:600px;">
    <tr><td align="center" style="padding:18px 12px;font-size:11px;line-height:1.5;color:#475569;">
      Sent by Webby. Replies to this address are not monitored.
    </td></tr>
  </table>
</td></tr>
</table>
</body>
</html>
"""

TEXT = """You're invited to Webby

Someone set up an account for you on Webby. Open the link below, pick a username and add a passkey.

  {url}

{terms}

If you weren't expecting this, ignore it.
"""


class SmtpNotConfigured(RuntimeError):
    pass


class InvalidAddress(ValueError):
    pass


def terms_text(max_uses, expires_at):
    if max_uses is None:
        uses = "This link works for any number of sign-ins."
    elif max_uses == 1:
        uses = "This link works for one sign-in."
    else:
        uses = f"This link works for up to {max_uses} sign-ins."
    if expires_at is None:
        return uses
    return f"{uses} It expires at {expires_at} UTC."


def render(url, max_uses=None, expires_at=None):
    terms = terms_text(max_uses, expires_at)
    return HTML.format(style=_STYLE, url=url, terms=terms), TEXT.format(url=url, terms=terms)


def check_address(value):
    address = (value or "").strip()
    if not address or "@" not in address or any(c in address for c in "\r\n<>"):
        raise InvalidAddress(value)
    return address


def build_invite(config, to, url, max_uses=None, expires_at=None):
    html, text = render(url, max_uses, expires_at)
    message = EmailMessage()
    message["Subject"] = SUBJECT
    message["From"] = formataddr((config.rp_name, config.smtp.sender))
    message["To"] = check_address(to)
    message.set_content(text)
    message.add_alternative(html, subtype="html")
    return message


def deliver(config, message):
    if not config.smtp.configured:
        raise SmtpNotConfigured("WEBBY_KONG_SMTP_HOST is not set")
    if config.smtp.tls == "ssl":
        with smtplib.SMTP_SSL(config.smtp.host, config.smtp.port, timeout=config.smtp.timeout,
                              context=ssl.create_default_context()) as smtp:
            _send(smtp, config, message)
    else:
        with smtplib.SMTP(config.smtp.host, config.smtp.port, timeout=config.smtp.timeout) as smtp:
            smtp.ehlo()
            if config.smtp.tls == "starttls":
                smtp.starttls(context=ssl.create_default_context())
                smtp.ehlo()
            _send(smtp, config, message)


def _send(smtp, config, message):
    if config.smtp.username:
        smtp.login(config.smtp.username, config.smtp.password or "")
    smtp.send_message(message)


def send_invite(config, to, url, max_uses=None, expires_at=None):
    message = build_invite(config, to, url, max_uses, expires_at)
    deliver(config, message)
    return message["To"]
