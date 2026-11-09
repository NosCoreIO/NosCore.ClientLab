"""Dev server for the browser build: the page, the wasm build output, and the user's own game folder (lazily, with
HTTP Range support). Nothing of the client is copied anywhere.

usage: serve.py --game /path/to/nostale [--build ../work/wasm/web] [--port 8077]
"""
import argparse
import base64
import hashlib
import http.server
import itertools
import json
import mimetypes
import os
import socket
import ssl
import secrets
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
GF_NOSTALE_GAME_ID = "dd4e22d6-00d1-44b9-8126-d8b40e0cd7c9"
# SHA-1/SHA-256 of the first certificate in GflessClient's public all_certs.pem.
# Its per-request User-Agent marker uses these digests; the private key is not needed here.
GF_CERT_SHA1 = "d68f9401f15791cc396d4d6af3b977bc58ad0002"
GF_CERT_SHA256 = "99025da70af1ef39d2acd049018887ef5140daebc6f11d80461bcf8d02f2d36b"


class GameforgeHttpError(RuntimeError):
    def __init__(self, status, headers, payload):
        self.status = status
        self.headers = headers
        self.payload = payload
        super().__init__(f"Gameforge returned HTTP {status}: {payload}")


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
        lock = threading.Lock()
        conn = {"sock": s, "buf": bytearray(), "closed": False, "lock": lock, "ready": threading.Condition(lock),
                "seen": time.monotonic()}
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
                with conn["ready"]:
                    conn["buf"] += data
                    conn["ready"].notify_all()
        except OSError:
            pass
        with conn["ready"]:
            conn["closed"] = True
            conn["ready"].notify_all()

    def send(self, cid, data):
        conn = self.conns.get(cid)
        if not conn or conn["closed"]:
            return False
        conn["seen"] = time.monotonic()
        conn["sock"].sendall(data)
        return True

    def recv(self, cid, wait=False):
        """With wait, blocks up to a second for data, so the page can long-poll instead of spinning."""
        conn = self.conns.get(cid)
        if not conn:
            return None
        conn["seen"] = time.monotonic()
        with conn["ready"]:
            if wait and not conn["buf"] and not conn["closed"]:
                conn["ready"].wait(timeout=1.0)
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


def gameforge_request(path, installation_id, user_agent, method="GET", body=None, bearer=None):
    base = "https://spark.gameforge.com/api"
    headers = {
        "Accept": "application/json",
        "Content-Type": "application/json",
        "TNT-Installation-Id": installation_id,
        "User-Agent": user_agent,
    }
    if path.startswith("v2/"):
        headers["GF-Installation-Id"] = installation_id

    def request(path, method="GET", body=None, bearer=None):
        request_headers = dict(headers)
        if bearer:
            request_headers["Authorization"] = f"Bearer {bearer}"
        data = None if body is None else json.dumps(body).encode()
        versioned_path = path if path.startswith("v2/") else f"v1/{path}"
        req = urllib.request.Request(f"{base}/{versioned_path}", data=data, method=method, headers=request_headers)
        try:
            with urllib.request.urlopen(req, timeout=20) as response:
                return json.loads(response.read())
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", "replace")[:1200]
            try:
                payload = json.loads(detail)
            except ValueError:
                payload = {"message": detail}
            raise GameforgeHttpError(error.code, error.headers, payload) from error

    return request(path, method, body, bearer)


def windows_gameforge_installation_id():
    if os.name != "nt":
        return None
    try:
        import winreg
        locations = (
            (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Gameforge4d\GameforgeClient\MainApp"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Gameforge4d\GameforgeClient\MainApp"),
        )
        for root, subkey in locations:
            try:
                with winreg.OpenKey(root, subkey) as key:
                    value, _ = winreg.QueryValueEx(key, "InstallationId")
                    if value:
                        return str(value)
            except OSError:
                continue
    except ImportError:
        pass
    return None


def gameforge_login(user, password, blackbox, installation_id, locale, user_agent, otp_code=None):
    body = {"email": user, "password": password, "locale": locale.replace("_", "-"), "blackbox": blackbox}
    if otp_code:
        body["otpCode"] = otp_code
    session = gameforge_request("v2/authProviders/credentials/sessions", installation_id,
                                user_agent, "POST", body)
    token = session.get("token")
    if not token:
        raise RuntimeError(f"Gameforge sign-in did not return a session token: {session}")

    # The upstream client loads the profile before listing its linked game accounts.
    gameforge_request("user/me", installation_id, user_agent, bearer=token)
    accounts = gameforge_request("user/accounts", installation_id, user_agent, bearer=token)
    entries = []
    if isinstance(accounts, dict):
        for key, value in accounts.items():
            if isinstance(value, dict):
                guls = value.get("guls", {})
                # Gameforge's account list also contains accounts for other games. The
                # upstream client only offers entries explicitly linked to NosTale.
                if not isinstance(guls, dict) or guls.get("game") != "nostale":
                    continue
                entries.append((str(value.get("id") or key), value))
            else:
                entries.append((str(key), value))
    elif isinstance(accounts, list):
        for item in accounts:
            if isinstance(item, dict):
                ident = item.get("platformGameAccountId") or item.get("id") or item.get("accountId")
                if ident is not None:
                    entries.append((str(ident), item))
    return token, [{"id": ident,
                    "name": value.get("displayName", value.get("name", ident)) if isinstance(value, dict) else ident}
                   for ident, value in entries]


def gameforge_captcha_images(challenge_id, locale):
    lang = locale.replace("_", "-")
    base = f"https://image-drop-challenge.gameforge.com/challenge/{urllib.parse.quote(challenge_id)}/{urllib.parse.quote(lang)}"
    headers = {"Accept": "*/*", "Content-Type": "application/json; charset=utf-8",
               "User-Agent": "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 "
                             "(KHTML, like Gecko) Chrome/72.0.3626.121 Safari/537.36",
               "Origin": "spark://www.gameforge.com", "Connection": "Keep-Alive"}

    def fetch(url, image=False):
        request_headers = dict(headers)
        if image:
            request_headers["Accept"] = "image/webp,image/apng,image/*,*/*;q=0.8"
        req = urllib.request.Request(url, headers=request_headers)
        with urllib.request.urlopen(req, timeout=20) as response:
            if image:
                return response.headers.get_content_type(), response.read()
            return json.loads(response.read())

    metadata = fetch(base)
    stamp = str(int(metadata["lastUpdated"]))
    images = {}
    for key, endpoint in (("prompt", "text"), ("icons", "drag-icons"), ("target", "drop-target")):
        media_type, data = fetch(f"{base}/{endpoint}?{stamp}", image=True)
        images[key] = f"data:{media_type};base64," + base64.b64encode(data).decode("ascii")
    return images


def gameforge_captcha_answer(challenge_id, locale, answer):
    lang = locale.replace("_", "-")
    url = f"https://image-drop-challenge.gameforge.com/challenge/{urllib.parse.quote(challenge_id)}/{urllib.parse.quote(lang)}"
    req = urllib.request.Request(url, data=json.dumps({"answer": int(answer)}).encode(), method="POST",
                                 headers={"Content-Type": "application/json; charset=utf-8", "Accept": "*/*",
                                          "User-Agent": "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 "
                                                        "(KHTML, like Gecko) Chrome/72.0.3626.121 Safari/537.36",
                                          "Origin": "spark://www.gameforge.com", "Connection": "Keep-Alive"})
    with urllib.request.urlopen(req, timeout=20) as response:
        return json.loads(response.read()).get("status") == "solved"


def gameforge_chrome_version():
    url = "http://dl.tnt.gameforge.com/tnt/final-ms3/clientversioninfo.json"
    with urllib.request.urlopen(url, timeout=20) as response:
        metadata = json.loads(response.read().decode("utf-8-sig"))
    return "C" + str(metadata["version"])


def gameforge_code_user_agent(chrome_version, installation_id, account_id):
    first_digit = next((int(char) for char in installation_id if char.isdigit()), 0)
    if first_digit % 2 == 0:
        hash_cert = GF_CERT_SHA256.encode("ascii")
        hash_version = hashlib.sha1(chrome_version.encode("utf-8")).hexdigest().encode("ascii")
        hash_installation = hashlib.sha256(installation_id.encode("utf-8")).hexdigest().encode("ascii")
        hash_account = hashlib.sha1(account_id.encode("utf-8")).hexdigest().encode("ascii")
        marker = hashlib.sha256(hash_cert + hash_version + hash_installation + hash_account).hexdigest()[:8]
    else:
        hash_cert = GF_CERT_SHA1.encode("ascii")
        hash_version = hashlib.sha256(chrome_version.encode("utf-8")).hexdigest().encode("ascii")
        hash_installation = hashlib.sha1(installation_id.encode("utf-8")).hexdigest().encode("ascii")
        hash_account = hashlib.sha256(account_id.encode("utf-8")).hexdigest().encode("ascii")
        marker = hashlib.sha256(hash_cert + hash_version + hash_installation + hash_account).hexdigest()[-8:]
    # Gameforge's thin-code endpoint expects the launcher UA suffix too. The
    # browser-style UA used for sessions/account lookup is not interchangeable
    # with the launcher UA used to mint a game code.
    return f"Chrome/{chrome_version} ({account_id[:2]}{marker}) GameforgeClient/2.1.22"


def encrypt_gameforge_blackbox(blackbox, gsid, account_id):
    key = hashlib.sha512(f"{gsid}-{account_id}".encode("utf-8")).hexdigest().encode("ascii")
    source = blackbox.encode("utf-8")
    encrypted = bytes(value ^ key[index % len(key)] ^ key[len(key) - (index % len(key)) - 1]
                      for index, value in enumerate(source))
    return base64.b64encode(encrypted).decode("ascii")


def gameforge_begin_launch(token, account_id, installation_id, user_agent, iovation_blackbox):
    if not iovation_blackbox:
        raise RuntimeError("Gameforge launch requires a fresh iovation blackbox.")
    game_session_id = str(uuid.uuid4())
    gsid = f"{game_session_id}-{secrets.randbelow(8999) + 1000}"
    iovation = gameforge_request("auth/iovation", installation_id, user_agent, "POST",
                                 {"accountId": account_id, "blackbox": iovation_blackbox,
                                  "type": "play_now"}, token)
    if iovation.get("status") != "ok":
        raise RuntimeError(f"Gameforge iovation check was not accepted: {iovation}")

    # The Gameforge endpoint rejects a code request sent immediately after iovation.
    time.sleep(1)
    blackbox_request = {"features": [secrets.randbelow(2147483646) + 1],
                        "installation": installation_id, "session": game_session_id}
    return gsid, blackbox_request


def gameforge_thin_code(token, account_id, installation_id, gsid, code_blackbox):
    if not all((code_blackbox, gsid)):
        raise RuntimeError("Gameforge launch requires a session-bound blackbox and game session ID.")
    chrome_version = gameforge_chrome_version()
    code_user_agent = gameforge_code_user_agent(chrome_version, installation_id, account_id)
    result = gameforge_request("auth/thin/codes", installation_id, code_user_agent, "POST",
                               {"platformGameAccountId": account_id, "gsid": gsid,
                                "blackbox": encrypt_gameforge_blackbox(code_blackbox, gsid, account_id),
                                "gameId": GF_NOSTALE_GAME_ID}, token)
    code = result.get("code")
    if not code:
        raise RuntimeError(f"Gameforge did not return a launch code: {result}")
    return code


def make_handler(game, build, relay, auth):
    class Handler(http.server.BaseHTTPRequestHandler):
        # Keep-alive: the worker reads game files in synchronous 1 MB range requests, and a new connection and thread
        # per request made each one cost several milliseconds more.
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def headers_common(self, ctype, length, immutable=False):
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(length))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "credentialless")
            self.send_header("Cache-Control", "max-age=31536000, immutable" if immutable else "no-cache")

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
                    out.append({"path": rel, "size": os.path.getsize(full), "mtime": int(os.path.getmtime(full))})
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

        def reply_json(self, status, value):
            return self.reply(status, json.dumps(value).encode(), "application/json")

        def finish_gameforge_login(self, token, accounts):
            if not accounts:
                return self.reply_json(400, {"error": "No linked game accounts were returned."})
            session_id = secrets.token_urlsafe(32)
            auth["login_sessions"][session_id] = {"token": token, "accounts": accounts,
                                                  "created": time.monotonic()}
            return self.reply_json(200, {"sessionId": session_id, "accounts": accounts})

        def gameforge_challenge(self, error, flow_id, email, password):
            error_types = error.payload.get("errorTypes", []) if isinstance(error.payload, dict) else []
            if "OTP_REQUIRED" in error_types:
                auth["pending_logins"][flow_id] = {"type": "otp", "email": email, "password": password,
                                                   "created": time.monotonic()}
                return self.reply_json(200, {"challenge": "otp", "flowId": flow_id,
                                             "message": "Enter the one-time code from your authenticator."})
            if "CHALLENGE_REQUIRED" in error_types:
                challenge_id = error.headers.get("gf-challenge-id", "").split(";")[0].strip()
                if not challenge_id:
                    return self.reply_json(502, {"error": "Gameforge requested a captcha but did not provide its challenge ID."})
                images = gameforge_captcha_images(challenge_id, auth["gf_locale"])
                auth["pending_logins"][flow_id] = {"type": "captcha", "email": email,
                                                   "password": password, "challenge_id": challenge_id,
                                                   "created": time.monotonic()}
                return self.reply_json(200, {"challenge": "captcha", "flowId": flow_id, "images": images})
            if "CREDENTIALS_INVALID" in error_types or error.status == 403:
                return self.reply_json(401, {"error": "Gameforge rejected those credentials."})
            return self.reply_json(502, {"error": str(error)})

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
                    data = relay.recv(cid, wait="wait" in q)
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
            if url.path in ("/auth/login", "/auth/challenge", "/auth/launch", "/auth/launch-code"):
                if self.command != "POST" or auth["provider"] != "gameforge":
                    return self.reply_json(404, {"error": "Gameforge browser login is not enabled"})
                try:
                    length = int(self.headers.get("Content-Length", 0))
                    if length > 16384:
                        return self.reply_json(413, {"error": "request is too large"})
                    payload = json.loads(self.rfile.read(length) or b"{}")
                    if url.path == "/auth/login":
                        email, password = payload.get("email", "").strip(), payload.get("password", "")
                        if not email or not password:
                            return self.reply_json(400, {"error": "Enter your Gameforge email and password."})
                        flow_id = secrets.token_urlsafe(32)
                        try:
                            token, accounts = gameforge_login(email, password, payload.get("blackbox", ""), auth["gf_installation_id"],
                                                              auth["gf_locale"], auth["gf_user_agent"])
                            return self.finish_gameforge_login(token, accounts)
                        except GameforgeHttpError as error:
                            return self.gameforge_challenge(error, flow_id, email, password)

                    if url.path == "/auth/challenge":
                        flow_id = payload.get("flowId")
                        pending = auth["pending_logins"].get(flow_id)
                        if not pending or time.monotonic() - pending["created"] > 900:
                            auth["pending_logins"].pop(flow_id, None)
                            return self.reply_json(401, {"error": "Challenge expired. Please sign in again."})
                        try:
                            if pending["type"] == "captcha":
                                solved = gameforge_captcha_answer(pending["challenge_id"], auth["gf_locale"],
                                                                  payload.get("answer", -1))
                                if not solved:
                                    images = gameforge_captcha_images(pending["challenge_id"], auth["gf_locale"])
                                    return self.reply_json(200, {"challenge": "captcha", "flowId": flow_id,
                                                                 "images": images,
                                                                 "message": "That answer was not accepted. Try again."})
                                otp_code = None
                            else:
                                otp_code = str(payload.get("otpCode", "")).strip()
                                if not otp_code:
                                    return self.reply_json(400, {"error": "Enter your one-time code."})
                            token, accounts = gameforge_login(pending["email"], pending["password"],
                                                              payload.get("blackbox", ""),
                                                              auth["gf_installation_id"], auth["gf_locale"],
                                                              auth["gf_user_agent"], otp_code)
                            auth["pending_logins"].pop(flow_id, None)
                            return self.finish_gameforge_login(token, accounts)
                        except GameforgeHttpError as error:
                            return self.gameforge_challenge(error, flow_id, pending["email"], pending["password"])

                    session_id, account_id = payload.get("sessionId"), str(payload.get("accountId", ""))
                    session = auth["login_sessions"].get(session_id)
                    if not session or time.monotonic() - session["created"] > 900:
                        auth["login_sessions"].pop(session_id, None)
                        return self.reply_json(401, {"error": "Sign-in expired. Please sign in again."})
                    if account_id not in {item["id"] for item in session["accounts"]}:
                        return self.reply_json(400, {"error": "Select one of your linked game accounts."})
                    if url.path == "/auth/launch":
                        gsid, blackbox_request = gameforge_begin_launch(
                            session["token"], account_id, auth["gf_installation_id"],
                            auth["gf_user_agent"], payload.get("iovationBlackbox", ""))
                        session["pending_launch"] = {"account_id": account_id, "gsid": gsid,
                                                      "created": time.monotonic()}
                        return self.reply_json(200, {"gsid": gsid, "blackboxRequest": blackbox_request})

                    pending_launch = session.get("pending_launch")
                    if (not pending_launch or pending_launch["account_id"] != account_id or
                            pending_launch["gsid"] != payload.get("gsid") or
                            time.monotonic() - pending_launch["created"] > 900):
                        session.pop("pending_launch", None)
                        return self.reply_json(401, {"error": "Launch request expired. Please retry."})
                    code = gameforge_thin_code(session["token"], account_id, auth["gf_installation_id"],
                                               pending_launch["gsid"], payload.get("codeBlackbox", ""))
                    auth["login_sessions"].pop(session_id, None)
                    return self.reply_json(200, {"code": code, "installationId": auth["gf_installation_id"]})
                except Exception as e:
                    return self.reply_json(502, {"error": str(e)})
            if url.path == "/auth-code":
                # Accept a short-lived ticket minted by an external launcher (for example,
                # an official Gameforge-compatible flow) without handling its credentials here.
                if auth["code"]:
                    return self.reply(200, auth["code"].encode(), "text/plain")
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
            # Game file URLs carry the file's modification time (worker.js), so a changed file gets a new URL and the
            # browser may keep every range it has read: a reload reads them from its cache instead of from here.
            versioned = url.path.startswith("/game/") and "v=" in url.query
            self.send_response(status)
            self.headers_common(ctype, end - start + 1, immutable=versioned)
            if versioned:
                self.send_header("Last-Modified", self.date_time_string(int(os.path.getmtime(full))))
                self.send_header("ETag", f'"{size:x}-{url.query}"')
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
    ap.add_argument("--build", default=os.environ.get(
        "CLIENTLAB_BUILD", os.path.abspath(os.path.join(HERE, "..", "..", "work", "wasm", "web"))))
    ap.add_argument("--port", type=int, default=8077)
    ap.add_argument("--auth-url", default="https://127.0.0.1:7001")
    ap.add_argument("--user", help="local NosCore account used to mint _NC_AUTH_CODE (dev only)")
    ap.add_argument("--password")
    ap.add_argument("--auth-code", default=os.environ.get("CLIENTLAB_AUTH_CODE"),
                    help="pre-issued launcher auth code (or set CLIENTLAB_AUTH_CODE)")
    ap.add_argument("--auth-provider", choices=("noscore", "gameforge"),
                    help="authentication provider (defaults to NosCore when --user is set, otherwise Gameforge)")
    ap.add_argument("--gf-installation-id", default=os.environ.get("TNT_INSTALLATION_ID"),
                    help="registered TNT installation ID (defaults to Windows Gameforge registry ID)")
    ap.add_argument("--gf-locale", default="en-GB", help="Gameforge locale, for example en-GB")
    ap.add_argument("--gf-user-agent", default=os.environ.get(
        "GF_USER_AGENT", "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/72.0.3626.121 Safari/537.36"),
                    help="Gameforge launcher User-Agent")
    ap.add_argument("--remap", action="append", default=[], metavar="FROM=TO",
                    help="relay the client's connections to port FROM to port TO instead")
    a = ap.parse_args()
    a.auth_provider = a.auth_provider or ("noscore" if a.user else "gameforge")
    a.gf_installation_id = a.gf_installation_id or windows_gameforge_installation_id()
    if a.auth_provider == "gameforge":
        if not a.gf_installation_id:
            ap.error("Gameforge auth requires a registered TNT installation ID. On Linux, set TNT_INSTALLATION_ID to the ID from an existing Gameforge installation.")
        try:
            uuid.UUID(a.gf_installation_id)
        except (ValueError, AttributeError, TypeError):
            ap.error("TNT installation ID must be a UUID from an existing Gameforge installation.")
    if a.auth_provider == "gameforge":
        auth = {"provider": "gameforge", "gf_installation_id": a.gf_installation_id,
                "gf_locale": a.gf_locale, "gf_user_agent": a.gf_user_agent,
                "login_sessions": {}, "pending_logins": {}, "code": None, "user": None}
    else:
        auth = {"provider": "noscore", "url": a.auth_url, "user": a.user, "password": a.password,
                "code": a.auth_code}
    remap = {int(f): int(t) for f, t in (r.split("=") for r in a.remap)}
    a.game, a.build = os.path.abspath(a.game), os.path.abspath(a.build)
    if not os.path.isdir(a.game):
        ap.error(f"game directory does not exist: {a.game}")
    if not os.path.isfile(os.path.join(a.build, "nostale.js")):
        ap.error(f"browser build not found in {a.build}; build with compile_all.py --web")
    server = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(a.game, a.build, Relay(remap), auth))
    print(f"http://127.0.0.1:{a.port}/  (game: {a.game})")
    server.serve_forever()


if __name__ == "__main__":
    main()
