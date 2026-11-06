"""licence_sign_pass.py <key.pem> <kid> <binding> <host:port>: prints a KCDS join pass for the
native roundtrip tests (licence_fixture.py), as the Master signs one: the base64url JSON body, a
dot and the base64url Ed25519 signature of the body. A Steam pass of a test account derived from
the binding (each test client its own player), valid for two minutes.
"""

import base64
import json
import secrets
import sys
import time

from cryptography.hazmat.primitives import serialization


def base64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def main():
    key_file, kid, binding, server = sys.argv[1:5]
    with open(key_file, "rb") as stream:
        key = serialization.load_pem_private_key(stream.read(), password=None)
    now = int(time.time())
    body = {
        "v": 1,
        "kid": kid,
        "store": "steam",
        "account": str(76561190000000000 + int(binding[:8], 16) % 1000000),
        "nonce": base64url(secrets.token_bytes(18)),
        "server": f"addr:{server}",
        "binding": binding,
        "iat": now,
        "exp": now + 120,
        "strength": "licence",
    }
    encoded = base64url(json.dumps(body, separators=(",", ":")).encode())
    signature = key.sign(encoded.encode())
    print(encoded + "." + base64url(signature))


if __name__ == "__main__":
    main()
