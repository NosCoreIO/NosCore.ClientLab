"""Downloads only NostaleClientX.exe from the current official patch manifest, checks it against the manifest's SHA-1
and prints build=, md5= and exe= lines (GitHub Actions step outputs). The executable stays in the given directory; it
is analysed, never committed."""
import hashlib
import json
import pathlib
import re
import sys
import urllib.request

MANIFEST_URL = "https://spark.gameforge.com/api/v1/patching/download/latest/nostale/default?locale=en&architecture=x64&branchToken"
USER_AGENT = "NosCore.ClientLab/0.1 (+https://github.com/NosCoreIO/NosCore.ClientLab)"


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def main():
    target = pathlib.Path(sys.argv[1])
    target.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(fetch(MANIFEST_URL))
    entry = next(item for item in manifest["entries"]
                 if not item.get("folder") and str(item["file"]).replace("\\", "/").lower() == "nostaleclientx.exe")
    path = str(entry["path"])
    if not path.startswith("/") or path.startswith("//"):
        raise SystemExit("The manifest has an unexpected download path for NostaleClientX.exe")
    # The patch CDN's certificate does not cover patches.gameforge.com; the SHA-1 below comes from the HTTPS manifest.
    data = fetch("http://patches.gameforge.com" + path)
    if hashlib.sha1(data).hexdigest() != str(entry["sha1"]).lower() or len(data) != int(entry["size"]):
        raise SystemExit("NostaleClientX.exe does not match the manifest")
    exe = target / "NostaleClientX.exe"
    exe.write_bytes(data)
    build = str(manifest.get("build", ""))
    if not re.fullmatch(r"[0-9A-Za-z._-]{1,64}", build):
        raise SystemExit("The manifest has an unexpected build number")
    print(f"build={build}")
    print(f"md5={hashlib.md5(data).hexdigest()}")
    print(f"exe={exe}")


if __name__ == "__main__":
    main()
