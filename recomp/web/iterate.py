"""Seed loop for the browser build: run drive.mjs, seed the first function nobody discovered, re-lift, rebuild, repeat.

The browser counterpart of iterate.ps1, for machines without the native MSVC build. serve.py must be running.

usage: iterate.py [--rounds 25] [--url http://127.0.0.1:8077/] [-- extra drive.mjs arguments]
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(HERE)
WORK = os.environ.get("NOSRECOMP_WORK", r"C:\tools\re\work" if os.name == "nt" else os.path.expanduser("~/re/work"))
MISSING = re.compile(r"no recompiled code for (?:guest address|indirect jump target) ([0-9a-f]{8})")


def main():
    argv = sys.argv[1:]
    extra = argv[argv.index("--") + 1:] if "--" in argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=25)
    ap.add_argument("--url", default="http://127.0.0.1:8077/")
    a = ap.parse_args(argv[:argv.index("--")] if "--" in argv else argv)

    gen = os.path.join(WORK, "recomp", "gen")
    seeds = os.path.join(WORK, "recomp", "seeds.txt")
    out = os.path.join(WORK, "drive", "iterate")
    open(seeds, "a").close()
    for rnd in range(1, a.rounds + 1):
        subprocess.run(["node", os.path.join(HERE, "drive.mjs"), "--url", a.url, "--out", out, *extra])
        with open(os.path.join(out, "full.log"), encoding="utf-8", errors="replace") as f:
            m = MISSING.search(f.read())
        if not m:
            print(f"round {rnd}: no missing-code stop")
            return
        addr = m.group(1)
        print(f"round {rnd}: missing {addr}")
        with open(seeds) as f:
            if addr in f.read().split():
                print(f"already seeded {addr}, giving up")
                sys.exit(1)
        with open(seeds, "a") as f:
            f.write(addr + "\n")
        subprocess.run([sys.executable, os.path.join(RECOMP, "lift.py"), os.path.join(WORK, "input", "NosCore.exe"),
                        os.path.join(WORK, "functions.txt"), gen, "--seeds", seeds], check=True)
        subprocess.run([sys.executable, os.path.join(RECOMP, "wasm", "compile_all.py"), "--web"], check=True)


if __name__ == "__main__":
    main()
