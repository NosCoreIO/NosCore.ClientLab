"""Compile the generated C and the portable host to WebAssembly, then link nostale.js (incremental).

usage: compile_all.py [--link] [--web [--web-out DIR]]
"""
import concurrent.futures as cf
import glob
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.environ.get("CLIENTLAB_GEN", os.path.join(HERE, "..", "..", "work", "recomp", "gen"))
OUT = os.environ.get("CLIENTLAB_WASM_OUT", os.path.join(HERE, "..", "..", "work", "wasm"))
RT = os.path.join(HERE, "..", "runtime")
PORT = os.path.join(HERE, "..", "port")
EMCC = os.environ.get("EMCC", shutil.which("emcc") or "emcc")
FLAGS = ["-O2", "-w", "-c", "-sSUPPORT_LONGJMP=0"]
LINK = ["-O2", "-sGLOBAL_BASE=536870912", "-sINITIAL_MEMORY=1024MB", "-sALLOW_MEMORY_GROWTH=1",
        "-sMAXIMUM_MEMORY=2048MB", "-sSTACK_SIZE=64MB", "-sNODERAWFS=1", "-sEXIT_RUNTIME=1",
        "-sSUPPORT_LONGJMP=0"]


def compile_one(src):
    obj = os.path.join(OUT, "obj", os.path.basename(src)[:-2] + ".o")
    deps = [src, os.path.join(RT, "rt.h")] + ([] if src.startswith(GEN) else [os.path.join(PORT, "port.h")] + glob.glob(os.path.join(PORT, "*.inc")))
    if os.path.exists(obj) and os.path.getmtime(obj) > max(os.path.getmtime(d) for d in deps):
        return src, 0, ""
    flags = FLAGS if src.startswith(GEN) else [f for f in FLAGS if f != "-w"] + ["-Wunused-function"]
    r = subprocess.run([EMCC, *flags, "-I", RT, "-I", PORT, src, "-o", obj], capture_output=True, text=True)
    if r.returncode == 0 and "warning" in r.stderr and not src.startswith(GEN):
        print(r.stderr[-1500:])
    return src, r.returncode, (r.stdout + r.stderr)[-1500:]


def main():
    global GEN, OUT, EMCC
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gen", default=GEN, help="directory containing lifted C files")
    ap.add_argument("--out", default=OUT, help="output directory for wasm objects and bundles")
    ap.add_argument("--emcc", default=EMCC, help="Emscripten compiler executable")
    ap.add_argument("--link", action="store_true", help="also link nostale.js for Node")
    ap.add_argument("--web", action="store_true", help="link the browser worker build")
    ap.add_argument("--web-out", help="browser build output directory")
    ap.add_argument("--profiling", action="store_true", help="preserve function names for CPU profiles")
    a = ap.parse_args()
    GEN, OUT, EMCC = os.path.abspath(a.gen), os.path.abspath(a.out), a.emcc
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
    # Only the objects of current sources: a renamed or deleted file must not leave its old object in the link.
    objs = sorted(os.path.join(OUT, "obj", os.path.basename(src)[:-2] + ".o") for src in srcs)
    if a.link:
        out = os.path.join(OUT, "nostale.js")
        r = subprocess.run([EMCC, *LINK, *objs, "-o", out], capture_output=True, text=True)
        print((r.stdout + r.stderr)[-3000:])
        if r.returncode:
            sys.exit(1)
        print("linked", out)
    if a.web:
        web_dir = a.web_out or os.path.join(OUT, "web")
        os.makedirs(web_dir, exist_ok=True)
        out = os.path.join(web_dir, "nostale.js")
        web = [f for f in LINK if not f.startswith("-sNODERAWFS")] + [
            "-sENVIRONMENT=worker", "-sFORCE_FILESYSTEM=1", "-sINVOKE_RUN=0",
            "-sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV"]
        # Keeps function names in the wasm for CPU profiles (drive.mjs --cpuprofile); the code is unchanged.
        if a.profiling:
            web.append("--profiling-funcs")
        r = subprocess.run([EMCC, *web, *objs, "-o", out], capture_output=True, text=True)
        print((r.stdout + r.stderr)[-3000:])
        if r.returncode:
            sys.exit(1)
        print("linked", out)


if __name__ == "__main__":
    main()
