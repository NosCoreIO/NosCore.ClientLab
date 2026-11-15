"""Portable launch identity imported from the user's official Gameforge launcher.

Encoding and refresh follow hatz2/GflessClient's BlackBox and Fingerprint classes.
Hardware/browser fields are retained from the captured identity.
"""
import base64
import argparse
import copy
import datetime
import email.utils
import json
import secrets
import time
import urllib.parse
import urllib.request


def decode_blackbox(blackbox):
    if not isinstance(blackbox, str) or not blackbox.startswith("tra:"):
        raise ValueError("Expected an official tra: blackbox")
    value = blackbox[4:]
    raw = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    data = bytes((v - (raw[i - 1] if i else 0)) % 256 for i, v in enumerate(raw))
    fields = json.loads(urllib.parse.unquote(data.decode("ascii")))
    if not isinstance(fields, list) or len(fields) != 30 or fields[0] != 12:
        raise ValueError("Unsupported Gameforge identity schema; capture a fresh version 12 identity")
    return fields


def encode_blackbox(fields):
    data = urllib.parse.quote(json.dumps(fields, ensure_ascii=False, separators=(",", ":")),
                              safe="-_!~*.'()").encode("ascii")
    result = bytearray()
    previous = 0
    for value in data:
        previous = (previous + value) % 256
        result.append(previous)
    return "tra:" + base64.urlsafe_b64encode(result).decode("ascii").rstrip("=")


class OfficialIdentity:
    def __init__(self, filename, installation_id):
        with open(filename, encoding="utf-8") as stream:
            data = json.load(stream)
        if not isinstance(data, dict) or data.get("installationId") != installation_id:
            raise ValueError("The captured Gameforge identity belongs to a different installation ID")
        fields = data.get("fingerprint")
        if not isinstance(fields, list) or len(fields) != 30 or fields[0] != 12:
            raise ValueError("Expected a captured version 12 Gameforge fingerprint")
        self.fields = copy.deepcopy(fields)
        self.user_agent = fields[26]
        # Validate the vector before any login takes place.
        vector, stamp = base64.b64decode(self.fields[25], validate=True).decode("ascii").rsplit(" ", 1)
        if not vector or not stamp.isdigit():
            raise ValueError("Invalid captured identity vector")

    def blackbox(self, request=None):
        # Obtain an actual server timestamp, as the official script/upstream do.
        req = urllib.request.Request("https://gameforge.com/tra/game1.js",
                                     headers={"User-Agent": self.user_agent})
        with urllib.request.urlopen(req, timeout=20) as response:
            server_date = email.utils.parsedate_to_datetime(response.headers["Date"])
        now = datetime.datetime.now(datetime.timezone.utc)
        fields = copy.deepcopy(self.fields)
        vector, stamp = base64.b64decode(fields[25]).decode("ascii").rsplit(" ", 1)
        milliseconds = int(time.time() * 1000)
        if milliseconds > int(stamp) + 1000:
            vector = vector[1:] + chr(secrets.randbelow(94) + 32)
        fields[25] = base64.b64encode(f"{vector} {milliseconds}".encode()).decode()
        self.fields[25] = fields[25]
        fields[21] = now.isoformat(timespec="milliseconds").replace("+00:00", "Z")
        fields[27] = server_date.astimezone(datetime.timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
        fields[28] = request
        return encode_blackbox(fields)


def main():
    parser = argparse.ArgumentParser(description="Import a blackbox collected by your official Gameforge launcher")
    parser.add_argument("--blackbox-file", required=True)
    parser.add_argument("--installation-id", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    with open(args.blackbox_file, encoding="utf-8") as stream:
        fields = decode_blackbox(stream.read().strip())
    request = fields[28]
    if isinstance(request, dict) and request.get("installation") != args.installation_id:
        parser.error("Captured identity and installation ID do not match")
    fields[28] = None
    with open(args.output, "x", encoding="utf-8") as stream:
        json.dump({"installationId": args.installation_id, "fingerprint": fields}, stream)
    print("Imported official launcher identity (no credentials or session tokens).")


if __name__ == "__main__":
    main()
