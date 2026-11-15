"""Install open-source build tools into the launcher data directory on first use."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import tarfile
import urllib.request
import zipfile

EMSDK_VERSION = "4.0.19"


def read_json(url):
    request = urllib.request.Request(url, headers={"User-Agent": "NosCore.ClientLab", "Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def download(url, destination, app, checksum=None):
    if not url.startswith("https://"):
        raise ValueError("Build tools must be downloaded over HTTPS")
    digest = hashlib.sha256()
    temporary = destination.with_suffix(destination.suffix + ".part")
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "NosCore.ClientLab"}), timeout=90) as response, temporary.open("wb") as output:
        size = int(response.headers.get("Content-Length", 0))
        done = 0
        for block in iter(lambda: response.read(1024 * 1024), b""):
            app.check_cancelled()
            output.write(block)
            digest.update(block)
            done += len(block)
            app.update(progress=done / size if size else 0)
    if checksum and digest.hexdigest() != checksum:
        raise RuntimeError("The downloaded tool checksum did not match")
    temporary.replace(destination)


def extract(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    root = destination.resolve()
    def checked(name):
        target = (root / name.replace("\\", "/")).resolve()
        if not target.is_relative_to(root):
            raise ValueError("Unsafe tool archive path")
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as stream:
            for member in stream.infolist():
                checked(member.filename)
                if (member.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError("Tool archive contains a symbolic link")
            stream.extractall(destination)
    else:
        with tarfile.open(archive) as stream:
            for member in stream.getmembers():
                checked(member.name)
            stream.extractall(destination, filter="data")


def install(directory, app):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    system = platform.system()
    machine = platform.machine().lower()
    arm = machine in ("arm64", "aarch64")
    if system not in ("Windows", "Linux", "Darwin"):
        raise ValueError("This operating system is not supported")
    os_name = {"Windows": "windows", "Darwin": "mac", "Linux": "linux"}[system]
    arch = "aarch64" if arm else "x64"
    app.update(stage="tools", message="Setting up a private Python runtime…", progress=0)
    python_root = directory / "python"
    python = python_root / ("python.exe" if system == "Windows" else "bin/python3")
    if not python.exists():
        triple = (("aarch64" if arm else "x86_64") + {"Windows": "-pc-windows-msvc", "Darwin": "-apple-darwin", "Linux": "-unknown-linux-gnu"}[system])
        release = read_json("https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest")
        assets = [a for a in release["assets"] if a["name"].startswith("cpython-3.12.") and
                  (triple + "-install_only_stripped.tar.gz") in a["name"]]
        if not assets:
            raise RuntimeError("No managed Python package is available for this architecture. Configure Python in Settings.")
        asset = sorted(assets, key=lambda a: a["name"])[-1]
        archive = directory / "python.tar.gz"
        checksum = (asset.get("digest") or "").removeprefix("sha256:") or None
        download(asset["browser_download_url"], archive, app, checksum)
        extract(archive, directory)
    if not python.exists():
        raise RuntimeError("The Python archive did not contain the expected executable")
    app.run([python, "-m", "pip", "install", "capstone==5.0.6", "pefile==2024.8.26"])
    app.update(message="Installing the Java analysis runtime…", progress=0)
    java_folder = directory / "java"
    java_executables = list(java_folder.rglob("java.exe" if system == "Windows" else "java")) if java_folder.exists() else []
    if not java_executables:
        assets = read_json(f"https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture={arch}&image_type=jdk&os={os_name}&vendor=eclipse")
        if not assets:
            raise RuntimeError("No Java runtime is available for this architecture")
        package = assets[0]["binary"]["package"]
        archive = directory / ("java.zip" if system == "Windows" else "java.tar.gz")
        download(package["link"], archive, app, package["checksum"])
        extract(archive, java_folder)
        java_executables = list(java_folder.rglob("java.exe" if system == "Windows" else "java"))
    java = next((path.parent.parent for path in java_executables if path.parent.name == "bin"), None)
    if not java:
        raise RuntimeError("Java was not found in the downloaded tool package")
    app.update(message="Installing Ghidra analysis tools…", progress=0)
    ghidra_parent = directory / "ghidra"
    analyzers = list(ghidra_parent.rglob("analyzeHeadless.bat" if system == "Windows" else "analyzeHeadless")) if ghidra_parent.exists() else []
    if not analyzers:
        # Match the project's Java 21 based analysis toolchain.
        release = read_json("https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/tags/Ghidra_12.0.4_build")
        asset = next(a for a in release["assets"] if "_PUBLIC" in a["name"] and a["name"].endswith(".zip"))
        archive = directory / "ghidra.zip"
        checksum = (asset.get("digest") or "").removeprefix("sha256:") or None
        download(asset["browser_download_url"], archive, app, checksum)
        extract(archive, ghidra_parent)
        analyzers = list(ghidra_parent.rglob("analyzeHeadless.bat" if system == "Windows" else "analyzeHeadless"))
    if not analyzers:
        raise RuntimeError("Ghidra did not contain the headless analysis tool")
    ghidra = analyzers[0].parent.parent
    if system != "Windows":
        for script in (ghidra / "support").iterdir():
            if script.is_file() and not script.suffix:
                script.chmod(script.stat().st_mode | 0o100)
    app.update(message="Installing the WebAssembly compiler…", progress=0)
    sdk = directory / f"emsdk-{EMSDK_VERSION}"
    if not sdk.exists():
        archive = directory / "emsdk.zip"
        download(f"https://github.com/emscripten-core/emsdk/archive/refs/tags/{EMSDK_VERSION}.zip", archive, app)
        extract(archive, directory)
    environment = os.environ.copy()
    environment["EMSDK_PYTHON"] = str(python)
    app.run([python, sdk / "emsdk.py", "install", EMSDK_VERSION], env=environment)
    app.run([python, sdk / "emsdk.py", "activate", EMSDK_VERSION], env=environment)
    # A tiny wrapper calls the managed interpreter, so no system Python is required.
    wrapper = directory / ("emcc.cmd" if system == "Windows" else "emcc")
    compiler = sdk / "upstream/emscripten/emcc.py"
    if system == "Windows":
        wrapper.write_text(f'@echo off\r\n"{python}" "{compiler}" %*\r\n', encoding="utf-8")
    else:
        import shlex
        wrapper.write_text(f"#!/bin/sh\nexec {shlex.quote(str(python))} {shlex.quote(str(compiler))} \"$@\"\n", encoding="utf-8")
        wrapper.chmod(0o755)
    return {"python": str(python), "java": str(java), "ghidra": str(ghidra), "emcc": str(wrapper)}
