"""Local native crash-capture test for the launcher's crash backend.

Usage: crash_capture_test.py <test_launcher_crash_capture_target.exe>

Runs only the small test program next to its crashpad_handler.exe; the game is
never started. A minimal HTTP receiver on 127.0.0.1 with an ephemeral port
stands in for our crash ingestion, so nothing leaves the machine:

1. With an empty DSN the program reports "disabled", creates no report
   database and the receiver gets no request.
2. With a loopback DSN the program crashes on purpose. The receiver must get
   the Crashpad upload for the configured project and key, containing a
   minidump and the release and environment tags.
"""

import gzip
import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

PUBLIC_KEY = "0123456789abcdef0123456789abcdef"
PROJECT_ID = "7"
RELEASE = b"kcds-crash-capture@0.0.1"
ENVIRONMENT = "crash-test"
UPLOAD_TIMEOUT_SECONDS = 60


class Upload:
    def __init__(self, path, query, headers, body):
        self.path = path
        self.query = query
        self.headers = headers
        self.body = body


class Receiver(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), ReceiverHandler)
        self.uploads = []
        self.lock = threading.Lock()
        self.received = threading.Event()

    def record(self, upload):
        with self.lock:
            self.uploads.append(upload)
        self.received.set()

    def handle_error(self, request, client_address):
        # The handler may drop its kept-alive connection after the upload.
        error = sys.exc_info()[1]
        if isinstance(error, ConnectionError):
            return
        super().handle_error(request, client_address)


class ReceiverHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def read_body(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            chunks = []
            while True:
                size_line = self.rfile.readline().strip()
                size = int(size_line.split(b";")[0], 16)
                if size == 0:
                    self.rfile.readline()
                    break
                chunks.append(self.rfile.read(size))
                self.rfile.readline()
            body = b"".join(chunks)
        else:
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
        if self.headers.get("Content-Encoding", "").lower() == "gzip":
            body = gzip.decompress(body)
        return body

    def do_POST(self):
        parsed = urllib.parse.urlsplit(self.path)
        body = self.read_body()
        self.server.record(
            Upload(parsed.path, urllib.parse.parse_qs(parsed.query), dict(self.headers), body)
        )
        reply = b'{"id":"00000000000000000000000000000000"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, format, *args):
        sys.stdout.write("receiver: " + (format % args) + "\n")


def child_environment():
    """The test's environment without proxies, so the upload stays local."""
    environment = dict(os.environ)
    for name in list(environment):
        if name.lower() in ("http_proxy", "https_proxy", "all_proxy"):
            del environment[name]
    return environment


def run_target(target, dsn, database):
    return subprocess.run(
        [target, dsn, database, ENVIRONMENT],
        env=child_environment(),
        capture_output=True,
        timeout=UPLOAD_TIMEOUT_SECONDS + 30,
    )


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


def check_disabled(target, receiver, scratch):
    database = os.path.join(scratch, "disabled-db")
    result = run_target(target, "", database)
    print("disabled run: exit=%d output=%r" % (result.returncode, result.stdout))
    if result.returncode != 3:
        fail("an empty DSN must leave reporting disabled (exit 3)")
    if os.path.exists(database):
        fail("disabled reporting must not create a report database")
    if receiver.uploads:
        fail("disabled reporting must not contact the receiver")


def check_crash_upload(target, receiver, scratch):
    port = receiver.server_address[1]
    dsn = "http://%s@127.0.0.1:%d/%s" % (PUBLIC_KEY, port, PROJECT_ID)
    database = os.path.join(scratch, "crash-db")
    result = run_target(target, dsn, database)
    print("crash run: exit=0x%08X output=%r" % (result.returncode & 0xFFFFFFFF, result.stdout))
    if b"crash reporting started" not in result.stdout:
        fail("crash reporting did not start: %r %r" % (result.stdout, result.stderr))
    if result.returncode in (0, 2, 3, 4, 5):
        fail("the test program did not crash")

    deadline = time.monotonic() + UPLOAD_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        with receiver.lock:
            uploads = list(receiver.uploads)
        for upload in uploads:
            if b"MDMP" in upload.body:
                return check_upload(upload)
        receiver.received.wait(0.5)
        receiver.received.clear()
    fail("no minidump upload arrived within %d seconds" % UPLOAD_TIMEOUT_SECONDS)


def check_upload(upload):
    print("upload: path=%s bytes=%d" % (upload.path, len(upload.body)))
    expected_paths = (
        "/api/%s/minidump/" % PROJECT_ID,
        "/api/%s/envelope/" % PROJECT_ID,
    )
    if upload.path not in expected_paths:
        fail("unexpected upload path " + upload.path)
    key_in_query = upload.query.get("sentry_key", [""])[0] == PUBLIC_KEY
    auth_header = ""
    for name, value in upload.headers.items():
        if name.lower() == "x-sentry-auth":
            auth_header = value
    if not key_in_query and ("sentry_key=" + PUBLIC_KEY) not in auth_header:
        fail("the upload does not carry the configured public key")
    if RELEASE not in upload.body:
        fail("the upload does not carry the release tag")
    if ENVIRONMENT.encode("ascii") not in upload.body:
        fail("the upload does not carry the environment tag")
    if b"KCDS-CAPTURE-LOG-TAIL" not in upload.body:
        fail("the upload does not carry the tail of the attached log")
    if b"[kcds: log truncated, last " not in upload.body:
        fail("the attached log was not marked as truncated")
    if b"KCDS-CAPTURE-LOG-HEAD" in upload.body:
        fail("the attached log was not bounded: its first line was uploaded")
    print("PASS: minidump with release, environment and bounded log received on 127.0.0.1")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    target = os.path.abspath(sys.argv[1])
    handler = os.path.join(os.path.dirname(target), "crashpad_handler.exe")
    if not os.path.isfile(handler):
        fail("crashpad_handler.exe is not next to the test program")

    receiver = Receiver()
    serving = threading.Thread(target=receiver.serve_forever, daemon=True)
    serving.start()
    scratch = tempfile.mkdtemp(prefix="kcds-crash-capture-")
    try:
        check_disabled(target, receiver, scratch)
        check_crash_upload(target, receiver, scratch)
    finally:
        receiver.shutdown()
        receiver.server_close()
        shutil.rmtree(scratch, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
