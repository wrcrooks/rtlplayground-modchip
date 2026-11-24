#!/usr/bin/env python3
"""The web server over TLS, with OpenSSL's client: run_https.py <test_httpd_tls>"""

import socket
import ssl
import subprocess
import sys
import time
import warnings

warnings.simplefilter("ignore")
PORT = 20443
failures = 0


def check(ok, what):
    global failures
    print("%s  %s" % ("PASS" if ok else "FAIL", what))
    failures += not ok


def get(ctx, session=None, body=b""):
    with socket.create_connection(("127.0.0.1", PORT), timeout=10) as s:
        with ctx.wrap_socket(s, session=session) as t:
            t.sendall(b"GET /status.json HTTP/1.1\r\n" + b"X-Filler: " + b"a" * 500 + b"\r\n\r\n")
            got = b""
            while True:
                try:
                    b = t.recv(4096)
                except ssl.SSLEOFError:
                    break
                if not b:
                    break
                got += b
            return got, t.version(), t.session, t.session_reused


def main():
    p = subprocess.Popen([sys.argv[1], str(PORT)], stdout=subprocess.DEVNULL)
    try:
        time.sleep(1)
        for ver in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
            ctx.minimum_version = ctx.maximum_version = ver
            got, v, sess, _ = get(ctx)
            check(got.startswith(b"HTTP/1.1 401"), "a request over " + v)
            got, v, _, reused = get(ctx, sess)
            check(got.startswith(b"HTTP/1.1 401") and reused, "  and again, resumed")
        ok = all(get(ctx)[0].startswith(b"HTTP/1.1 401") for _ in range(20))
        check(ok, "twenty connections")
        ctx.minimum_version = ssl.TLSVersion.TLSv1
        ctx.maximum_version = ssl.TLSVersion.TLSv1_1
        ctx.set_ciphers("ALL:@SECLEVEL=0")
        try:
            get(ctx)
            check(False, "TLS 1.1 refused")
        except (ssl.SSLError, OSError):
            check(True, "TLS 1.1 refused")
    finally:
        p.kill()
    print("FAILED" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
