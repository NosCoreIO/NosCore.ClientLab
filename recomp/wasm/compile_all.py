"""Compile the generated C to WebAssembly objects in parallel (incremental)."""
import concurrent.futures as cf
import glob
import os
import subprocess
import sys
import time

GEN = r"C:\tools\re\work\recomp\gen"
OUT = r"C:\tools\re\work\wasm\obj"
RT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runtime")
EMCC = r"C:\tools\emsdk\upstream\emscripten\emcc.exe"
FLAGS = ["-O1", "-w", "-c", "-pthread"]


def one(src):
    obj = os.path.join(OUT, os.path.basename(src)[:-2] + ".o")
    if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src):
        return src, 0, ""
    r = subprocess.run([EMCC, *FLAGS, "-I", RT, src, "-o", obj], capture_output=True, text=True)
    return src, r.returncode, (r.stdout + r.stderr)[-800:]


def main():
    os.makedirs(OUT, exist_ok=True)
    t = time.time()
    srcs = sorted(glob.glob(os.path.join(GEN, "*.c")))
    fails = []
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex:
        for src, rc, msg in ex.map(one, srcs):
            if rc:
                fails.append((src, msg))
    print(f"{len(srcs) - len(fails)}/{len(srcs)} compiled in {time.time() - t:.0f}s")
    for s, m in fails[:5]:
        print(s, m)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
