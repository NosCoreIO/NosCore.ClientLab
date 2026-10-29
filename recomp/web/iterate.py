"""Seed loop for the browser build: run drive.mjs, seed the first function nobody discovered, re-lift, rebuild, repeat.

The browser counterpart of iterate.ps1, for machines without the native MSVC build. serve.py must be running.

usage: iterate.py [--rounds 25] [--url http://127.0.0.1:8077/] [--before "<command>"] [-- extra drive.mjs arguments]

--before runs a shell command ahead of each round, for instance restarting the NosCore servers so the world server
does not still hold the previous round's session ("account name is currently in use").
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
    ap.add_argument("--before")
    a = ap.parse_args(argv[:argv.index("--")] if "--" in argv else argv)

    gen = os.path.join(WORK, "recomp", "gen")
    seeds = os.path.join(WORK, "recomp", "seeds.txt")
    out = os.path.join(WORK, "drive", "iterate")
    open(seeds, "a").close()
    for rnd in range(1, a.rounds + 1):
        if a.before:
            subprocess.run(a.before, shell=True, check=True)
        log_path = os.path.join(out, "full.log")
        if os.path.exists(log_path):
            os.remove(log_path)
        drive = subprocess.run(["node", os.path.join(HERE, "drive.mjs"), "--url", a.url, "--out", out, *extra])
        if not os.path.exists(log_path):
            print(f"round {rnd}: drive.mjs wrote no log (exit {drive.returncode})")
            sys.exit(1)
        with open(log_path, encoding="utf-8", errors="replace") as f:
            log = f.read()
        m = MISSING.search(log)
        if not m and drive.returncode:
            print(f"round {rnd}: drive.mjs failed (exit {drive.returncode}) before any missing-code stop")
            print("\n".join(l for l in log.splitlines() if l.startswith("FATAL"))[:2000])
            sys.exit(1)
        if not m:
            print(f"round {rnd}: no missing-code stop")
            print("\n".join(l for l in log.splitlines() if l.startswith("FATAL"))[:2000])
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
