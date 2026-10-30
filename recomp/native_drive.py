"""Drives the native client (NosTaleRecomp.exe) through the same steps as web/drive.mjs, over the runtime's control
channel (runtime/rt_control.c): login, server and channel, character, a click on the ground, a chat line, the game
windows by hotkey, GM teleports. Screenshots come from the backbuffer, so the window can be covered or minimised.

usage: python recomp/native_drive.py --game <folder with the patched NosCore.exe> [--out dir] [--maps 2,145]
       [--auth-url http://127.0.0.1:7001 --user admin --password test] [--port 7790] [--exe NosTaleRecomp.exe]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "web"))
from serve import fetch_auth_code  # noqa: E402  (same two calls as NosCore.Launcher)

WORK = os.environ.get("NOSRECOMP_WORK", r"C:\tools\re\work")

# drive.mjs positions on the client shown at 1680x1050; scaled to the backbuffer.
REF_W, REF_H = 1680, 1050
SERVER_ROW = (788, 422)
CHANNEL_ROW = (1068, 422)
CHARACTER_SLOT = (1170, 70)
START_BUTTON = (840, 965)
GROUND = (1000, 520)

VK = {"enter": 0x0D, "escape": 0x1B, "f9": 0x78}


class Control:
    def __init__(self, port, seconds=180):
        deadline = time.time() + seconds
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise RuntimeError(f"no control channel on port {port}")
                time.sleep(1)
        self.sock.settimeout(None)
        self.buf = b""

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("control channel closed (client exited?)")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace")

    def _bytes(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("control channel closed")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def cmd(self, text):
        self.sock.sendall(text.encode() + b"\n")
        return self._line()

    def ok(self, text):
        r = self.cmd(text)
        if not r.startswith("ok"):
            raise RuntimeError(f"{text!r}: {r}")
        return r.split()[1:]

    def mark(self):
        return int(self.ok("mark")[0])

    def log(self, offset=0):
        _, end, n = self.cmd(f"log {offset}").split()
        return int(end), self._bytes(int(n)).decode(errors="replace")

    def wait(self, offset, text, seconds):
        r = self.cmd(f"wait {offset} {int(seconds * 1000)} {text}").split()
        return r[0] == "ok"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, help="game folder with the patched NosCore.exe")
    ap.add_argument("--exe", default=os.path.join(WORK, "recomp", "out", "NosTaleRecomp.exe"))
    ap.add_argument("--out", default=os.path.join(WORK, "drive", "native"))
    ap.add_argument("--port", type=int, default=7790)
    ap.add_argument("--auth-url", default="http://127.0.0.1:7001")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="test")
    ap.add_argument("--maps", default="2")
    ap.add_argument("--keep", action="store_true", help="leave the client running at the end")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    # Without libcef.dll, as in the browser build: CEF's helper processes fail to start a GPU process under the
    # recompiled client and take it down.
    env = dict(os.environ, NOSRECOMP_CONTROL=str(a.port), NOSRECOMP_NO_CEF="1", _NC_AUTH_CODE=fetch_auth_code(a.auth_url, a.user, a.password))
    client = subprocess.Popen([a.exe, os.path.join(a.game, "NosCore.exe"), "gf", "0"], env=env)
    started = time.time()
    step = lambda name: print(f"[{time.strftime('%H:%M:%S')}] {name}", flush=True)
    c = None
    failed = False
    try:
        c = Control(a.port)
        size = lambda: tuple(int(v) for v in c.ok("size"))
        at = lambda p: (round(p[0] * size()[0] / REF_W), round(p[1] * size()[1] / REF_H))
        shot = lambda name: c.ok(f"shot {os.path.join(a.out, name)}")
        click = lambda p, button="left": c.ok("click %d %d %s" % (*at(p), button))
        key = lambda k: c.ok(f"key {VK[k] if k in VK else ord(k.upper())}")

        step("waiting for login")
        if not c.wait(0, "closesocket", 300):
            raise RuntimeError("login did not complete")
        print(f"login screen after {time.time() - started:.0f} s, backbuffer {size()[0]}x{size()[1]}")
        time.sleep(4)
        shot("1-servers.png")

        step("server and channel")
        click(SERVER_ROW)
        time.sleep(2)
        mark = c.mark()
        click(CHANNEL_ROW)
        if not c.wait(mark, "connect 127.0.0.1:1337", 60):
            raise RuntimeError("no world connection")
        time.sleep(3)
        shot("2-characters.png")

        step("enter world")
        click(CHARACTER_SLOT)
        time.sleep(2)
        click(START_BUTTON)
        time.sleep(15)
        shot("3-world.png")

        step("click ground")
        mark = c.mark()
        click(GROUND)
        time.sleep(4)
        shot("4-after-click.png")
        print("\n".join(l for l in c.log(mark)[1].splitlines() if l.startswith("send ")) or "no packet sent")

        step("chat")
        mark = c.mark()
        key("enter")
        time.sleep(0.8)
        c.ok("type hello from the native client")
        time.sleep(0.5)
        shot("5-typing.png")
        key("enter")
        time.sleep(3)
        shot("6-after-chat.png")
        print("\n".join(l for l in c.log(mark)[1].splitlines() if l.startswith("send ")) or "no packet sent")

        step("game windows")
        mark = c.mark()
        key("escape")
        time.sleep(0.5)
        for k in "ickplfm":
            key(k)
            time.sleep(1.5)
            shot(f"7-window-{k}.png")
            key(k)
            time.sleep(0.5)
        errors = [l for l in c.log(mark)[1].splitlines() if any(w in l for w in ("FATAL", "exception", "fatal"))]
        print("\n".join(errors[:12]) or "windows: no errors")

        for m in a.maps.split(","):
            step(f"teleport to map {m}")
            mark = c.mark()
            key("enter")
            time.sleep(0.5)
            c.ok(f"type $Teleport {m}")
            key("enter")
            time.sleep(0.5)
            key("escape")
            time.sleep(14)
            shot(f"8-map-{m}.png")
            errors = [l for l in c.log(mark)[1].splitlines() if any(w in l for w in ("FATAL", "exception", "fatal"))]
            print("\n".join(errors[:12]) or f"map {m}: no errors")
    except Exception as e:  # noqa: BLE001
        print("error:", e)
        failed = True
    finally:
        if c:
            try:
                with open(os.path.join(a.out, "full.log"), "w", encoding="utf-8") as f:
                    f.write(c.log(0)[1])
            except Exception:  # noqa: BLE001
                pass
        if not a.keep:
            client.kill()
            subprocess.run(["taskkill", "/f", "/im", os.path.basename(a.exe)], capture_output=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
