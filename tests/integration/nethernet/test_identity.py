import asyncio
import base64

import pytest
from cryptography.hazmat.primitives.asymmetric import ec
from harness import PLAYER_IDENTITY, connect_player
from nethernet import ConnectionFailed, SessionError
from nethernet.identity import IdentitySigner, encode_cpk, generate_operator_key


def run(coroutine):
    return asyncio.run(asyncio.wait_for(coroutine, 60))


def jwk(key: ec.EllipticCurvePrivateKey) -> dict:
    numbers = key.public_key().public_numbers()

    def coordinate(value: int) -> str:
        return base64.urlsafe_b64encode(value.to_bytes(48, "big")).decode().rstrip("=")

    return {"crv": "P-384", "kty": "EC", "x": coordinate(numbers.x), "y": coordinate(numbers.y)}


class RewritingSigner:
    def __init__(self, rewrite):
        self.rewrite = rewrite

    def sign(self, sdp: str) -> str:
        return self.rewrite(PLAYER_IDENTITY.sign(sdp))


def refusal(proxy, identity_signer) -> SessionError:
    async def body():
        with pytest.raises(ConnectionFailed) as failure:
            await connect_player(proxy.url, identity_signer)
        return failure.value.error

    return run(body())


def test_an_offer_without_an_identity_is_refused(proxy):
    assert refusal(proxy, None) == SessionError.IDENTITY_NOT_ALLOWED


def test_an_offer_whose_fingerprints_changed_after_signing_is_refused(proxy):
    signer = RewritingSigner(lambda sdp: sdp.replace("a=fingerprint:sha-256 ", "a=fingerprint:sha-256 00:"))
    assert refusal(proxy, signer) == SessionError.IDENTITY_NOT_ALLOWED


def test_an_offer_signed_under_a_key_other_than_its_cpk_is_refused(proxy):
    cpk = encode_cpk(generate_operator_key().public_key())
    signer = IdentitySigner(generate_operator_key(), domain="self", claims={"cpk": cpk})
    assert refusal(proxy, signer) == SessionError.IDENTITY_NOT_ALLOWED


def test_an_offer_with_a_json_web_key_cpk_is_refused(proxy):
    key = generate_operator_key()
    signer = IdentitySigner(key, domain="self", claims={"cpk": jwk(key)})
    assert refusal(proxy, signer) == SessionError.IDENTITY_NOT_ALLOWED


def test_an_offer_with_a_token_over_16_kib_is_refused(proxy):
    signer = IdentitySigner(generate_operator_key(), domain="self", claims={"padding": "x" * 16 * 1024})
    assert refusal(proxy, signer) == SessionError.IDENTITY_NOT_ALLOWED
