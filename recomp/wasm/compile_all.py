"""Compile the generated C and the portable host to WebAssembly, then link nostale.js (incremental).

usage: compile_all.py [--link]
"""
import concurrent.futures as cf
import glob
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = r"C:\tools\re\work\recomp\gen"
OUT = r"C:\tools\re\work\wasm"
RT = os.path.join(HERE, "..", "runtime")
PORT = os.path.join(HERE, "..", "port")
EMCC = r"C:\tools\emsdk\upstream\emscripten\emcc.exe"
FLAGS = ["-O1", "-w", "-c", "-sSUPPORT_LONGJMP=0"]
LINK = ["-O1", "-sGLOBAL_BASE=536870912", "-sINITIAL_MEMORY=1024MB", "-sALLOW_MEMORY_GROWTH=1",
        "-sMAXIMUM_MEMORY=2048MB", "-sSTACK_SIZE=64MB", "-sNODERAWFS=1", "-sEXIT_RUNTIME=1",
        "-sSUPPORT_LONGJMP=0"]


def compile_one(src):
    obj = os.path.join(OUT, "obj", os.path.basename(src)[:-2] + ".o")
    deps = [src, os.path.join(RT, "rt.h")] + ([] if src.startswith(GEN) else [os.path.join(PORT, "port.h")])
    if os.path.exists(obj) and os.path.getmtime(obj) > max(os.path.getmtime(d) for d in deps):
        return src, 0, ""
    flags = FLAGS if src.startswith(GEN) else [f for f in FLAGS if f != "-w"] + ["-Wunused-function"]
    r = subprocess.run([EMCC, *flags, "-I", RT, "-I", PORT, src, "-o", obj], capture_output=True, text=True)
    if r.returncode == 0 and "warning" in r.stderr and not src.startswith(GEN):
        print(r.stderr[-1500:])
    return src, r.returncode, (r.stdout + r.stderr)[-1500:]


def main():
    os.makedirs(os.path.join(OUT, "obj"), exist_ok=True)
    t = time.time()
    srcs = sorted(glob.glob(os.path.join(GEN, "*.c"))) + [os.path.join(RT, "rt.c")] + sorted(glob.glob(os.path.join(PORT, "*.c")))
    fails = []
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex:
        for src, rc, msg in ex.map(compile_one, srcs):
            if rc:
                fails.append((src, msg))
    print(f"{len(srcs) - len(fails)}/{len(srcs)} compiled in {time.time() - t:.0f}s")
    for s, m in fails[:5]:
        print(s, m)
    if fails:
        sys.exit(1)
    if "--link" in sys.argv:
        objs = sorted(glob.glob(os.path.join(OUT, "obj", "*.o")))
        out = os.path.join(OUT, "nostale.js")
        r = subprocess.run([EMCC, *LINK, *objs, "-o", out], capture_output=True, text=True)
        print((r.stdout + r.stderr)[-3000:])
        if r.returncode:
            sys.exit(1)
        print("linked", out)


if __name__ == "__main__":
    main()
