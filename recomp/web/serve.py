"""Dev server for the browser build: the page, the wasm build output, and the user's own game folder (lazily, with
HTTP Range support). Nothing of the client is copied anywhere.

usage: serve.py --game "C:/Program Files (x86)/Nostale" [--build C:/tools/re/work/wasm/web] [--port 8077]
"""
import argparse
import http.server
import json
import mimetypes
import os
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))


def make_handler(game, build):
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

        def serve(self, head):
            if urllib.parse.urlparse(self.path).path == "/game-index.json":
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

        def do_HEAD(self):
            self.serve(True)

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--build", default=r"C:\tools\re\work\wasm\web")
    ap.add_argument("--port", type=int, default=8077)
    a = ap.parse_args()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(a.game, a.build))
    print(f"http://127.0.0.1:{a.port}/  (game: {a.game})")
    server.serve_forever()


if __name__ == "__main__":
    main()
