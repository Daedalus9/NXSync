"""Exercise the real curl multi client against an isolated, trusted HTTPS fixture."""
import base64
import http.server
import json
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
from urllib.parse import parse_qs


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, code, body, location=None):
        data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        if location:
            self.send_header("Location", location)
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        scenario = self.server.scenario
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode()
        assert "Authorization" not in self.headers
        assert self.headers["User-Agent"].startswith("NXSync/")
        base = self.server.base
        if self.path == "/nextcloud/index.php/login/v2":
            self.server.starts += 1
            assert not body
            result = {"login": base + "/login/flow/test", "poll": {
                "token": "poll-secret+&=%/", "endpoint": base + "/login/v2/poll"}}
            if scenario == "cross_poll": result["poll"]["endpoint"] = "https://example.invalid/poll"
            if scenario == "cross_login": result["login"] = "https://example.invalid/login"
            if scenario == "http_login": result["login"] = "http://localhost/login"
            if scenario == "missing": del result["poll"]
            if scenario == "redirect": return self.reply(307, {}, base + "/unexpected")
            if scenario == "duplicate": return self.reply(200, '{"login":"a","login":"b"}')
            if scenario == "nested": return self.reply(200, '{"x":' + '[' * 20 + '0' + ']' * 20 + '}')
            if scenario == "oversize": return self.reply(200, "x" * 17000)
            if scenario == "malformed": return self.reply(200, '<html>not JSON</html>')
            if scenario == "nul": return self.reply(200, '{"login":"bad\\u0000url"}')
            return self.reply(200, result)
        if self.path == "/nextcloud/login/v2/poll":
            self.server.polls += 1
            assert parse_qs(body) == {"token": ["poll-secret+&=%/"]}
            if scenario == "poll_redirect": return self.reply(307, {}, base + "/unexpected")
            if self.server.polls == 1: return self.reply(404, {})
            if scenario == "retry" and self.server.polls == 2: return self.reply(503, {})
            return self.reply(200, {"server": "https://example.invalid" if scenario == "cross_server" else base,
                "loginName": "person@example.test", "appPassword": "" if scenario == "empty_password" else "synthetic-app-password"})
        self.server.unexpected += 1
        self.reply(500, {})

    def do_GET(self):
        self.server.users += 1
        assert self.path == "/nextcloud/ocs/v2.php/cloud/user?format=json"
        assert self.headers["OCS-APIRequest"] == "true"
        assert self.headers["Authorization"] == "Basic " + base64.b64encode(
            b"person@example.test:synthetic-app-password").decode()
        if self.server.scenario == "user_retry" and self.server.users == 1: return self.reply(429, {})
        if self.server.scenario == "user_redirect": return self.reply(302, {}, "https://example.invalid/")
        self.reply(200, {"ocs": {"meta": {"status": "ok"}, "data": {
            "id": "" if self.server.scenario == "missing_uid" else "opaque-user-id"}}})


with tempfile.TemporaryDirectory(prefix="nxsync-login-") as tmp:
    cert, key = Path(tmp) / "cert.pem", Path(tmp) / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=localhost",
        "-addext", "subjectAltName=DNS:localhost"], check=True, capture_output=True)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    server.base = f"https://localhost:{server.server_port}/nextcloud"
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for scenario in ("ok", "retry", "user_retry", "cancel", "expire", "untrusted", "cross_poll",
                "cross_login", "http_login", "missing", "redirect", "poll_redirect", "user_redirect",
                "duplicate", "nested", "oversize", "malformed", "nul", "cross_server", "empty_password", "missing_uid"):
            server.scenario = scenario
            server.starts = server.polls = server.users = server.unexpected = 0
            subprocess.run([sys.argv[1], server.base, str(cert), scenario], check=True, timeout=25)
            assert server.unexpected == 0, scenario
            if scenario == "untrusted": assert server.starts == 0
            if scenario in ("cancel", "expire"): assert server.users == 0
            if scenario in ("ok", "retry", "user_retry"):
                assert server.polls >= 2 and server.users >= 1
            print("PASS", scenario, flush=True)
    finally:
        server.shutdown()
        server.server_close()
