"""Thin wrappers over py_webauthn.

The browser gets exactly what `options_to_json` produces (base64url strings for challenge, user.id and credential
ids); this module is the only place that imports `webauthn`, so a missing runtime dependency fails at startup.
"""
import base64
import json
import secrets

from webauthn import (generate_authentication_options, generate_registration_options, options_to_json,
                      verify_authentication_response, verify_registration_response)
from webauthn.helpers import base64url_to_bytes
from webauthn.helpers.exceptions import InvalidAuthenticationResponse, InvalidRegistrationResponse
from webauthn.helpers.structs import (AttestationConveyancePreference, AuthenticatorSelectionCriteria,
                                      PublicKeyCredentialDescriptor, ResidentKeyRequirement,
                                      UserVerificationRequirement)

# ES256 first (every platform authenticator has it), RS256 as the Windows Hello fallback.
ALGORITHMS = [-7, -257]
TIMEOUT_MS = 60000


class VerificationFailed(Exception):
    """The browser's answer did not verify (bad challenge, origin, signature, ...)."""


def b64url(data):
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def new_challenge():
    """32 random bytes, base64url-encoded for storage; return (challenge_b64, challenge_bytes)."""
    raw = secrets.token_bytes(32)
    return b64url(raw), raw


def _descriptors(credential_ids):
    return [PublicKeyCredentialDescriptor(id=base64url_to_bytes(credential_id)) for credential_id in credential_ids]


def registration_options(config, user_id, username, existing_ids=()):
    challenge_b64, challenge = new_challenge()
    options = generate_registration_options(
        rp_id=config.rp_id,
        rp_name=config.rp_name,
        user_id=user_id,
        user_name=username,
        user_display_name=username,
        challenge=challenge,
        timeout=TIMEOUT_MS,
        attestation=AttestationConveyancePreference.NONE,
        authenticator_selection=AuthenticatorSelectionCriteria(
            resident_key=ResidentKeyRequirement.REQUIRED,
            require_resident_key=True,
            user_verification=UserVerificationRequirement.PREFERRED,
        ),
        exclude_credentials=_descriptors(existing_ids),
        supported_pub_key_algs=ALGORITHMS,
    )
    return challenge_b64, json.loads(options_to_json(options))


def authentication_options(config):
    challenge_b64, challenge = new_challenge()
    options = generate_authentication_options(
        rp_id=config.rp_id,
        challenge=challenge,
        timeout=TIMEOUT_MS,
        allow_credentials=[],  # discoverable: the authenticator picks the passkey, no username is typed
        user_verification=UserVerificationRequirement.PREFERRED,
    )
    return challenge_b64, json.loads(options_to_json(options))


def verify_registration(config, challenge_b64, credential):
    try:
        return verify_registration_response(
            credential=credential,
            expected_challenge=base64url_to_bytes(challenge_b64),
            expected_rp_id=config.rp_id,
            expected_origin=config.origin,
            require_user_verification=False,
        )
    except (InvalidRegistrationResponse, ValueError) as err:
        raise VerificationFailed(str(err)) from err


def verify_authentication(config, challenge_b64, credential, stored):
    """`stored` is the credentials row: its public key and sign counter are the trust anchor."""
    try:
        return verify_authentication_response(
            credential=credential,
            expected_challenge=base64url_to_bytes(challenge_b64),
            expected_rp_id=config.rp_id,
            expected_origin=config.origin,
            credential_public_key=bytes(stored["public_key"]),
            credential_current_sign_count=stored["sign_count"],
            require_user_verification=False,
        )
    except (InvalidAuthenticationResponse, ValueError) as err:
        raise VerificationFailed(str(err)) from err
