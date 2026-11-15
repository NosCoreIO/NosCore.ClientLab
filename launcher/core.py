"""Versioned, content-addressed installation and local recompilation pipeline."""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import threading
import time
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
MANIFEST_URL = "https://spark.gameforge.com/api/v1/patching/download/latest/nostale/default?locale=en&architecture=x64&branchToken"
USER_AGENT = "NosCore.ClientLab/0.1 (+https://github.com/NosCoreIO/NosCore.ClientLab)"


def data_directory():
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
    elif platform.system() == "Darwin":
        base = Path.home() / "Library/Application Support"
    else:
        base = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    return base / "NosCore.ClientLab"


def file_hash(path, algorithm="sha256"):
    result = hashlib.new(algorithm)
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2), encoding="utf-8")
    temporary.replace(path)


def relative_file(value):
    path = PurePosixPath(str(value).replace("\\", "/"))
    if (not str(value) or path.is_absolute() or ".." in path.parts or
            any(":" in part or part.endswith((".", " ")) for part in path.parts)):
        raise ValueError("The update manifest contains an unsafe file path")
    return path


def patch_entries(manifest):
    entries = []
    seen = set()
    for item in manifest.get("entries", []):
        if item.get("folder"):
            continue
        name = str(relative_file(item["file"]))
        if name.casefold() in seen:
            raise ValueError("The update manifest contains duplicate file paths")
        seen.add(name.casefold())
        digest = str(item["sha1"]).lower()
        if not re.fullmatch(r"[a-f0-9]{40}", digest):
            raise ValueError("The update manifest contains an invalid checksum")
        remote = str(item.get("path", ""))
        if not remote.startswith("/") or remote.startswith("//") or urllib.parse.urlsplit(remote).netloc:
            raise ValueError("The update manifest contains an invalid download path")
        size = int(item["size"])
        if size < 0:
            raise ValueError("The update manifest contains an invalid file size")
        entries.append({"file": name, "sha1": digest, "size": size, "path": remote})
    if not entries:
        raise ValueError("Gameforge returned an empty patch manifest")
    return entries


class Cancelled(Exception):
    pass


class Launcher:
    def __init__(self, directory=None):
        self.directory = Path(directory or data_directory()).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.cancelled = threading.Event()
        self.process = None
        self.game_server = None
        self.game_url = None
        self.settings = {"import_dir": "", "identity_file": "", "installation_id": "",
                         "provider": "gameforge", "python": "", "ghidra": "", "java": "", "emcc": ""}
        config = self.directory / "settings.json"
        if config.exists():
            self.settings.update(json.loads(config.read_text(encoding="utf-8")))
        self.state = {"busy": False, "stage": "idle", "message": "Make yourself at home.", "progress": 0,
                      "logs": [], "error": None, "remote": None, "plan": None,
                      "platform": platform.system(), "active": None, "running": False}
        active = self.directory / "active.json"
        if active.exists():
            self.state["active"] = json.loads(active.read_text(encoding="utf-8"))
        self.manifest = None

    def snapshot(self):
        with self.lock:
            result = copy.deepcopy(self.state)
            result["settings"] = dict(self.settings)
            result["data_dir"] = str(self.directory)
            result["tools"] = self.tool_status()
            return result

    def update(self, **values):
        with self.lock:
            self.state.update(values)

    def log(self, text):
        with self.lock:
            self.state["logs"] = (self.state["logs"] + [str(text)])[-500:]

    def check_cancelled(self):
        if self.cancelled.is_set():
            raise Cancelled("Operation cancelled. Your previous build is still available.")

    def save_settings(self, values):
        with self.lock:
            if self.state["busy"] or self.game_server:
                raise ValueError("Close the game or wait for the current task before changing settings")
            for key in self.settings:
                if key in values:
                    self.settings[key] = str(values[key]).strip()
            if self.settings["provider"] not in ("gameforge", "noscore"):
                raise ValueError("Unknown authentication provider")
            atomic_json(self.directory / "settings.json", self.settings)

    def start(self, action):
        actions = {"check": self.check_updates, "prepare": self.prepare, "tools": self.install_tools}
        if action not in actions:
            raise ValueError("Unknown task")
        with self.lock:
            if self.state["busy"] or self.game_server:
                raise ValueError("Finish the current task or close the game first")
            self.state.update(busy=True, error=None, progress=0)
            self.cancelled.clear()
        def work():
            try:
                actions[action]()
            except Cancelled as error:
                self.update(stage="idle", message=str(error))
            except Exception as error:
                self.log(str(error))
                self.update(stage="error", error=str(error), message="This step needs attention.")
            finally:
                self.process = None
                self.update(busy=False)
        threading.Thread(target=work, daemon=True).start()

    def cancel(self):
        self.cancelled.set()
        if self.process and self.process.poll() is None:
            self.process.terminate()

    def tool_status(self):
        return {key: bool(self.settings.get(key) and Path(self.settings[key]).exists())
                for key in ("python", "ghidra", "java", "emcc")}

    def install_tools(self):
        from launcher.toolchain import install
        installed = install(self.directory / "tools", self)
        self.settings.update(installed)
        atomic_json(self.directory / "settings.json", self.settings)
        self.update(stage="idle", progress=1, message="Build tools are ready.")

    def run(self, command, *, env=None):
        self.check_cancelled()
        self.log("Running " + Path(str(command[0])).name)
        options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
        process = subprocess.Popen([str(v) for v in command], cwd=self.directory,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                   encoding="utf-8", errors="replace", env=env, **options)
        self.process = process
        for line in process.stdout:
            self.log(line.rstrip())
            if self.cancelled.is_set():
                process.terminate()
        code = process.wait()
        self.check_cancelled()
        if code:
            raise RuntimeError(f"{Path(str(command[0])).name} exited with code {code}. Open Activity for details.")

    def check_updates(self):
        self.update(stage="checking", message="Checking the latest game files…")
        with urllib.request.urlopen(urllib.request.Request(MANIFEST_URL, headers={"User-Agent": USER_AGENT}), timeout=40) as response:
            manifest = json.load(response)
        entries = patch_entries(manifest)
        identity = hashlib.sha256(json.dumps(entries, sort_keys=True).encode()).hexdigest()[:20]
        self.manifest = {"id": identity, "build": str(manifest.get("build", identity)), "entries": entries}
        total = sum(item["size"] for item in entries)
        missing = sum(item["size"] for item in entries if not (self.directory / "content" / item["sha1"]).exists())
        atomic_json(self.directory / "latest.json", self.manifest)
        self.update(stage="idle", progress=1, remote={"id": identity, "build": self.manifest["build"]},
                    plan={"files": len(entries), "total_bytes": total, "download_bytes": missing},
                    message="Latest version found. Ready when you are.")

    def download(self, entry):
        self.check_cancelled()
        store = self.directory / "content" / entry["sha1"]
        store.parent.mkdir(exist_ok=True)
        if store.exists() and store.stat().st_size == entry["size"] and file_hash(store, "sha1") == entry["sha1"]:
            return store
        temporary = store.with_suffix(".part")
        imported = Path(self.settings["import_dir"]) / entry["file"] if self.settings["import_dir"] else None
        if imported and imported.is_file() and imported.stat().st_size == entry["size"] and file_hash(imported, "sha1") == entry["sha1"]:
            shutil.copyfile(imported, temporary)
        else:
            url = "https://patches.gameforge.com" + entry["path"]
            request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(request, timeout=90) as response, temporary.open("wb") as output:
                for chunk in iter(lambda: response.read(1024 * 1024), b""):
                    self.check_cancelled()
                    output.write(chunk)
        if temporary.stat().st_size != entry["size"] or file_hash(temporary, "sha1") != entry["sha1"]:
            raise RuntimeError(f"Checksum mismatch for {entry['file']}. The downloaded file was not installed.")
        temporary.replace(store)
        return store

    def prepare(self):
        if not all(self.tool_status().values()):
            raise ValueError("Set up build tools first, or configure existing tools in Settings")
        self.check_updates()
        manifest = self.manifest
        version = self.directory / "versions" / manifest["id"]
        client = version / "client"
        client.mkdir(parents=True, exist_ok=True)
        self.update(stage="downloading", progress=0, message="Downloading and verifying game files…")
        total = max(sum(item["size"] for item in manifest["entries"]), 1)
        completed = 0
        for item in manifest["entries"]:
            store = self.download(item)
            target = client / item["file"]
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists() or file_hash(target, "sha1") != item["sha1"]:
                temporary = target.with_suffix(target.suffix + ".installing")
                # Separate files keep the immutable content store safe from game writes.
                shutil.copyfile(store, temporary)
                temporary.replace(target)
            completed += item["size"]
            self.update(progress=completed / total, message=f"Preparing {item['file']}")
        executable = client / "NostaleClientX.exe"
        if not executable.exists():
            raise RuntimeError("The official manifest did not include NostaleClientX.exe")
        import pefile
        pe = pefile.PE(str(executable), fast_load=True)
        if pe.FILE_HEADER.Machine != 0x14C:
            raise RuntimeError("This client is not 32-bit x86. ClientLab needs a compatible recompiler update.")
        digest = file_hash(executable)
        # Pin the exact image; never run a rebuilt runtime against a different install's executable.
        pinned = version / "compiled-client.exe"
        shutil.copyfile(executable, pinned)
        ghidra = Path(self.settings["ghidra"])
        analyzer = ghidra / "support" / ("analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless")
        analysis = version / "analysis"
        analysis.mkdir(exist_ok=True)
        functions = version / "functions.txt"
        environment = os.environ.copy()
        environment["JAVA_HOME"] = self.settings["java"]
        environment["PATH"] = str(Path(self.settings["java"]) / "bin") + os.pathsep + environment.get("PATH", "")
        python = self.settings["python"]
        self.update(stage="analysing", progress=0, message="Preparing the new client. This can take a while…")
        self.run([analyzer, analysis, "ClientLab", "-import", pinned, "-overwrite", "-processor", "x86:LE:32:default",
                  "-cspec", "borlanddelphi", "-scriptPath", ROOT / "tools/ghidra", "-postScript", "ExportFunctions.java", functions], env=environment)
        if not functions.exists() or not functions.stat().st_size:
            raise RuntimeError("Analysis did not produce a function list; the current version remains selected")
        generated = version / "generated"
        seeds = version / "seeds.txt"
        seeds.write_text(f"{pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.AddressOfEntryPoint:x}\n")
        self.update(stage="translating", progress=0, message="Building a runtime for this client version…")
        self.run([python, ROOT / "recomp/lift.py", pinned, functions, generated, "--seeds", seeds])
        table = (generated / "table.c").read_text()
        entry = pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        if f"0x{entry:08x}u" not in table:
            raise RuntimeError("The client entry point could not be translated. A compatibility update is needed.")
        self.update(stage="compiling", progress=0, message="Compiling the game runtime…")
        self.run([python, ROOT / "recomp/wasm/compile_all.py", "--gen", generated, "--out", version / "wasm",
                  "--web", "--emcc", self.settings["emcc"]], env=environment)
        build = version / "wasm/web"
        for name in ("nostale.js", "nostale.wasm"):
            with (build / name).open("rb") as stream:
                if not stream.read(4):
                    raise RuntimeError("The compiler produced an empty runtime")
        record = {"id": manifest["id"], "build": manifest["build"], "executable_sha256": digest,
                  "client": str(client), "executable": str(pinned), "runtime": str(build), "created": time.time()}
        atomic_json(version / "build.json", record)
        atomic_json(self.directory / "active.json", record)
        self.update(active=record, stage="ready", progress=1, message="Your build is ready to launch.")

    def launch(self):
        with self.lock:
            if self.state["busy"]:
                raise ValueError("Wait for the current task to finish")
            if self.game_server and self.game_server.poll() is None:
                return self.game_url
            active = self.state["active"]
            if not active:
                raise ValueError("Prepare a client build first")
        if file_hash(active["executable"]) != active["executable_sha256"]:
            raise ValueError("The client executable changed. Prepare a new matching runtime before playing.")
        import socket
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        command = [self.settings["python"], str(ROOT / "recomp/web/serve.py"), "--game", active["client"],
                   "--build", active["runtime"], "--client-exe", active["executable"], "--port", str(port),
                   "--auth-provider", self.settings["provider"]]
        if self.settings["provider"] == "gameforge":
            if self.settings["identity_file"]:
                identity = json.loads(Path(self.settings["identity_file"]).read_text(encoding="utf-8"))
                installation = self.settings["installation_id"] or identity.get("installationId")
                if installation:
                    command += ["--gf-installation-id", installation]
                command += ["--gf-identity-file", self.settings["identity_file"]]
            elif self.settings["installation_id"]:
                command += ["--gf-installation-id", self.settings["installation_id"]]
        options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
        self.game_log = (self.directory / "game.log").open("w", encoding="utf-8")
        self.game_server = subprocess.Popen(command, stdout=self.game_log, stderr=subprocess.STDOUT, **options)
        base = f"http://127.0.0.1:{port}/"
        for _ in range(100):
            if self.game_server.poll() is not None:
                self.game_log.close()
                self.game_server = None
                raise RuntimeError("The game server could not start. Check the installation identity and game.log.")
            try:
                with urllib.request.urlopen(base + "build/nostale.js", timeout=1) as response:
                    if response.status == 200:
                        break
            except OSError:
                time.sleep(.1)
        else:
            self.close_game()
            raise RuntimeError("The game server did not become ready")
        self.game_url = base + ("?login=gameforge" if self.settings["provider"] == "gameforge" else "")
        self.update(running=True)
        return self.game_url

    def close_game(self):
        if self.game_server:
            self.game_server.terminate()
            try:
                self.game_server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.game_server.kill()
            self.game_server = None
            self.game_log.close()
        self.game_url = None
        self.update(running=False)
