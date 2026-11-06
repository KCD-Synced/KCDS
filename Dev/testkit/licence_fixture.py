"""A KCDS Master for the native roundtrip tests: every server requires a legal game,
so the tests' clients join with join passes the way players do.

The fixture makes a throwaway Ed25519 key and a self-signed certificate for 127.0.0.1, serves
GET /v1/licence/keys over HTTPS on a free loopback port as the Master does, and writes
licence.json into the server's resources folder naming it. The server's Node.js host trusts the
certificate through NODE_EXTRA_CA_CERTS (only in the environment given to the test's server). The
test client (client_resource_probe) gets each join pass from licence_sign_pass.py through
KCDS_TEST_PASS_SIGNER, for the server nonce of its handshake.

    with LicenceFixture(work, resources_folder) as licence:
        subprocess.Popen([...server...], env=licence.server_env())
        subprocess.Popen([...probe...], env=licence.probe_env())
"""

import base64
import datetime
import http.server
import ipaddress
import json
import os
from pathlib import Path
import ssl
import sys
import threading

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, ed25519
from cryptography.x509.oid import NameOID

HERE = Path(__file__).resolve().parent
KEY_ID = "testkit"


class LicenceFixture:
    def __init__(self, work, resources):
        self.work = Path(work)
        self.resources = Path(resources)
        self.server = None
        self.thread = None

    def __enter__(self):
        key = ed25519.Ed25519PrivateKey.generate()
        self.key_file = self.work / "licence-test-key.pem"
        self.key_file.write_bytes(
            key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
        )
        public = key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw
        )
        keys = json.dumps(
            {
                "keys": [{"kid": KEY_ID, "ed25519": base64.b64encode(public).decode()}],
                "address": "127.0.0.1",
            }
        ).encode()
        self.cert_file, cert_key_file = self.certificate()

        class Keys(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path != "/v1/licence/keys":
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(keys)))
                self.end_headers()
                self.wfile.write(keys)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Keys)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.cert_file), str(cert_key_file))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        master = f"https://127.0.0.1:{self.server.server_address[1]}"
        (self.resources / "licence.json").write_text(
            json.dumps({"master": master}), encoding="utf8"
        )
        return self

    def __exit__(self, *exc):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
        return False

    def certificate(self):
        key = ec.generate_private_key(ec.SECP256R1())
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "kcds-testkit-master")])
        now = datetime.datetime.now(datetime.timezone.utc)
        certificate = (
            x509.CertificateBuilder()
            .subject_name(name)
            .issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(minutes=5))
            .not_valid_after(now + datetime.timedelta(hours=2))
            .add_extension(
                x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]),
                critical=False,
            )
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .sign(key, hashes.SHA256())
        )
        cert_file = self.work / "licence-test-master.pem"
        key_file = self.work / "licence-test-master-key.pem"
        cert_file.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
        key_file.write_bytes(
            key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
        )
        return cert_file, key_file

    def server_env(self):
        env = dict(os.environ)
        env["NODE_EXTRA_CA_CERTS"] = str(self.cert_file)
        return env

    def probe_env(self):
        env = dict(os.environ)
        signer = HERE / "licence_sign_pass.py"
        env["KCDS_TEST_PASS_SIGNER"] = f'"{sys.executable}" "{signer}" "{self.key_file}" {KEY_ID}'
        return env
