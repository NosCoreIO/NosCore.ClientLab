"""Authenticated loopback API used by the launcher's local UI."""
import hmac
import http.server
import json
from pathlib import Path
import secrets
import threading
import urllib.parse

from launcher.core import Launcher


def create_server(app: Launcher, port=0):
    token = secrets.token_urlsafe(32)

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, status, data, content_type="application/json"):
            if not isinstance(data, bytes):
                data = json.dumps(data).encode()
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(data)

        def authorized(self):
            supplied = self.headers.get("X-ClientLab-Token", "")
            return hmac.compare_digest(supplied, token)

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            if path == "/api/state":
                if not self.authorized():
                    return self.reply(403, {"error": "Unauthorized"})
                return self.reply(200, app.snapshot())
            return self.reply(404, {"error": "Not found"})

        def do_POST(self):
            if not self.authorized():
                return self.reply(403, {"error": "Unauthorized"})
            try:
                length = int(self.headers.get("Content-Length", 0))
                if not 0 <= length <= 32768:
                    return self.reply(413, {"error": "Request too large"})
                payload = json.loads(self.rfile.read(length) or b"{}")
                path = urllib.parse.urlsplit(self.path).path
                if path == "/api/settings":
                    app.save_settings(payload)
                elif path == "/api/task":
                    app.start(payload["action"])
                elif path == "/api/cancel":
                    app.cancel()
                elif path == "/api/play":
                    return self.reply(200, {"url": app.launch()})
                elif path == "/api/close-game":
                    app.close_game()
                else:
                    return self.reply(404, {"error": "Unknown action"})
                return self.reply(200, {"ok": True})
            except Exception as error:
                return self.reply(400, {"error": str(error)})

    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.api_token = token
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server
