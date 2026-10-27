"""Dev server for the browser build: the page, the wasm build output, and the user's own game folder (lazily, with
HTTP Range support). Nothing of the client is copied anywhere.

usage: serve.py --game "C:/Program Files (x86)/Nostale" [--build C:/tools/re/work/wasm/web] [--port 8077]
"""
import argparse
import http.server
import itertools
import json
import mimetypes
import os
import socket
import ssl
import threading
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))


class Relay:
    """TCP connections held for the worker, which cannot open sockets itself and never yields to an event loop:
    it drives them with synchronous HTTP requests (/net/connect, /net/send, /net/recv, /net/close)."""

    IDLE_SECONDS = 20

    def __init__(self, remap):
        self.remap = remap
        self.ids = itertools.count(1)
        self.conns = {}
        self.lock = threading.Lock()
        threading.Thread(target=self._reaper, daemon=True).start()

    def _reaper(self):
        """A reloaded or closed page never sends /net/close; without this its sockets stay open and the server keeps
        the account logged in."""
        while True:
            time.sleep(5)
            now = time.monotonic()
            with self.lock:
                stale = [cid for cid, c in self.conns.items() if now - c["seen"] > self.IDLE_SECONDS]
            for cid in stale:
                self.close(cid)

    def connect(self, host, port):
        port = self.remap.get(port, port)
        s = socket.create_connection((host, port), timeout=5)
        s.settimeout(None)
        cid = next(self.ids)
        conn = {"sock": s, "buf": bytearray(), "closed": False, "lock": threading.Lock(), "seen": time.monotonic()}
        with self.lock:
            self.conns[cid] = conn
        threading.Thread(target=self._reader, args=(conn,), daemon=True).start()
        return cid

    def _reader(self, conn):
        try:
            while True:
                data = conn["sock"].recv(65536)
                if not data:
                    break
                with conn["lock"]:
                    conn["buf"] += data
        except OSError:
            pass
        conn["closed"] = True

    def send(self, cid, data):
        conn = self.conns.get(cid)
        if not conn or conn["closed"]:
            return False
        conn["seen"] = time.monotonic()
        conn["sock"].sendall(data)
        return True

    def recv(self, cid):
        conn = self.conns.get(cid)
        if not conn:
            return None
        conn["seen"] = time.monotonic()
        with conn["lock"]:
            data, conn["buf"] = bytes(conn["buf"]), bytearray()
        if not data and conn["closed"]:
            return None
        return data

    def close(self, cid):
        conn = self.conns.pop(cid, None)
        if conn:
            try:
                conn["sock"].close()
            except OSError:
                pass


def fetch_auth_code(auth_url, user, password):
    """Same two calls as NosCore.Launcher: credentials -> session token, then a short-lived auth code."""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    def post(path, body, bearer=None):
        req = urllib.request.Request(f"{auth_url.rstrip('/')}/{path}", data=json.dumps(body).encode(), method="POST",
                                     headers={"Content-Type": "application/json", "Accept": "application/json"})
        if bearer:
            req.add_header("Authorization", f"Bearer {bearer}")
        with urllib.request.urlopen(req, context=ctx, timeout=10) as r:
            return json.loads(r.read())

    session = post("api/v1/auth/thin/sessions", {"identity": user, "password": password, "gfLang": "EN", "locale": "en-GB", "mfa": None})
    code = post("api/v1/auth/thin/codes", {"platformGameAccountId": session["platformGameAccountId"]}, session["token"])
    return code["code"]


def make_handler(game, build, relay, auth):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def headers_common(self, ctype, length):
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(length))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.send_header("Cache-Control", "no-cache")

        def resolve(self):
            path = urllib.parse.unquote(urllib.parse.urlparse(self.path).path)
            if path in ("/", "/index.html"):
                return os.path.join(HERE, "index.html")
            if path == "/worker.js":
                return os.path.join(HERE, "worker.js")
            if path.startswith("/build/"):
                return os.path.join(build, path[len("/build/"):])
            if path.startswith("/game/"):
                full = os.path.realpath(os.path.join(game, path[len("/game/"):]))
                return full if full.startswith(os.path.realpath(game)) else None
            return None

        def game_index(self):
            out = []
            for root, _, files in os.walk(game):
                for f in files:
                    full = os.path.join(root, f)
                    rel = os.path.relpath(full, game).replace("\\", "/")
                    out.append({"path": rel, "size": os.path.getsize(full)})
            body = json.dumps(out).encode()
            self.send_response(200)
            self.headers_common("application/json", len(body))
            self.end_headers()
            self.wfile.write(body)

        def reply(self, status, body=b"", ctype="application/octet-stream"):
            self.send_response(status)
            self.headers_common(ctype, len(body))
            self.end_headers()
            self.wfile.write(body)

        def net(self, url):
            q = urllib.parse.parse_qs(url.query)
            op = url.path[len("/net/"):]
            try:
                if op == "connect":
                    return self.reply(200, str(relay.connect(q["host"][0], int(q["port"][0]))).encode(), "text/plain")
                cid = int(q["id"][0])
                if op == "send":
                    body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                    return self.reply(200 if relay.send(cid, body) else 410)
                if op == "recv":
                    data = relay.recv(cid)
                    return self.reply(410) if data is None else self.reply(200, data)
                if op == "close":
                    relay.close(cid)
                    return self.reply(200)
            except OSError as e:
                return self.reply(502, str(e).encode(), "text/plain")
            self.reply(404)

        def serve(self, head):
            url = urllib.parse.urlparse(self.path)
            if url.path.startswith("/net/"):
                return self.net(url)
            if url.path == "/auth-code":
                if not auth["user"]:
                    return self.reply(404)
                try:
                    return self.reply(200, fetch_auth_code(auth["url"], auth["user"], auth["password"]).encode(), "text/plain")
                except Exception as e:
                    return self.reply(502, str(e).encode(), "text/plain")
            if url.path == "/game-index.json":
                return self.game_index()
            full = self.resolve()
            if not full or not os.path.isfile(full):
                self.send_error(404)
                return
            size = os.path.getsize(full)
            ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
            if full.endswith(".wasm"):
                ctype = "application/wasm"
            start, end, status = 0, size - 1, 200
            rng = self.headers.get("Range")
            if rng and rng.startswith("bytes="):
                a, _, b = rng[6:].partition("-")
                start = int(a) if a else max(0, size - int(b))
                end = int(b) if a and b else size - 1
                end = min(end, size - 1)
                status = 206
            self.send_response(status)
            self.headers_common(ctype, end - start + 1)
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            if head:
                return
            with open(full, "rb") as f:
                f.seek(start)
                left = end - start + 1
                while left > 0:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)

        def do_GET(self):
            self.serve(False)

        def do_POST(self):
            self.serve(False)

        def do_HEAD(self):
            self.serve(True)

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--build", default=r"C:\tools\re\work\wasm\web")
    ap.add_argument("--port", type=int, default=8077)
    ap.add_argument("--auth-url", default="https://127.0.0.1:7001")
    ap.add_argument("--user", help="local NosCore account used to mint _NC_AUTH_CODE (dev only)")
    ap.add_argument("--password")
    ap.add_argument("--remap", action="append", default=[], metavar="FROM=TO",
                    help="relay the client's connections to port FROM to port TO instead")
    a = ap.parse_args()
    auth = {"url": a.auth_url, "user": a.user, "password": a.password}
    remap = {int(f): int(t) for f, t in (r.split("=") for r in a.remap)}
    server = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(a.game, a.build, Relay(remap), auth))
    print(f"http://127.0.0.1:{a.port}/  (game: {a.game})")
    server.serve_forever()


if __name__ == "__main__":
    main()
