"""Query the decompiled C export: callers, callees, bodies and string hits per function."""
import argparse
import glob
import os
import re

HEADER = re.compile(r"^// (\S+) @ ([0-9a-f]{8})$")
CALL = re.compile(r"\b([A-Za-z_@][\w@]*)\s*\(")


def load(decomp_dir):
    funcs = {}
    for path in sorted(glob.glob(os.path.join(decomp_dir, "*.c"))):
        name = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = HEADER.match(line.rstrip("\n"))
                if m:
                    name = m.group(1).replace(".", "_")
                    funcs[name] = {"addr": m.group(2), "body": []}
                elif name:
                    funcs[name]["body"].append(line)
    for name, info in funcs.items():
        text = "".join(info["body"])
        info["text"] = text
        info["calls"] = {c for c in CALL.findall(text) if c in funcs and c != name}
    return funcs


def resolve(funcs, key):
    key = key.replace(".", "_")
    if key in funcs:
        return key
    addr = key.lower().removeprefix("0x").zfill(8)
    for name, info in funcs.items():
        if info["addr"] == addr:
            return name
    raise SystemExit(f"unknown function {key}")


def main():
    p = argparse.ArgumentParser()
    work = os.environ.get("NOSRECOMP_WORK", r"C:\tools\re\work" if os.name == "nt" else os.path.expanduser("~/re/work"))
    p.add_argument("--dir", default=os.path.join(work, "decomp"))
    p.add_argument("cmd", choices=["callers", "callees", "body", "grep"])
    p.add_argument("target")
    p.add_argument("-d", "--depth", type=int, default=1)
    a = p.parse_args()
    funcs = load(a.dir)

    if a.cmd == "grep":
        rx = re.compile(a.target)
        for name, info in funcs.items():
            if rx.search(info["text"]):
                print(f"{info['addr']} {name}")
        return

    name = resolve(funcs, a.target)
    if a.cmd == "body":
        print(f"// {name} @ {funcs[name]['addr']}")
        print(funcs[name]["text"])
        return

    callers = {}
    if a.cmd == "callers":
        for n, info in funcs.items():
            for c in info["calls"]:
                callers.setdefault(c, set()).add(n)

    def walk(n, depth, seen):
        nxt = funcs[n]["calls"] if a.cmd == "callees" else callers.get(n, set())
        for m in sorted(nxt):
            print("  " * depth + f"{funcs[m]['addr']} {m}")
            if depth < a.depth and m not in seen:
                walk(m, depth + 1, seen | {m})

    print(f"{funcs[name]['addr']} {name}")
    walk(name, 1, {name})


if __name__ == "__main__":
    main()
