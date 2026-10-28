"""Differential test of the lifter: each distinct instruction shape of the client is lifted to C and run on random
states in a native harness, and the same bytes run under Unicorn from the same states. Registers, defined flags and
the memory sandbox must match.

usage: difftest.py <client exe> <functions.txt> <work dir> [--cases N] [--only MNEMONIC]
"""
import argparse
import os
import random
import struct
import subprocess
import sys
from collections import defaultdict

import capstone
from capstone import x86_const as X
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError
from unicorn import x86_const as U

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import lift  # noqa: E402

SANDBOX, SANDBOX_SIZE = 0x10000000, 0x1000
CODE_AT = 0x20000000
FPU_IN, FPU_OUT = SANDBOX + 0xF00, SANDBOX + 0xF80
REGS = [U.UC_X86_REG_EAX, U.UC_X86_REG_ECX, U.UC_X86_REG_EDX, U.UC_X86_REG_EBX,
        U.UC_X86_REG_ESP, U.UC_X86_REG_EBP, U.UC_X86_REG_ESI, U.UC_X86_REG_EDI]
REG_INDEX = {X.X86_REG_EAX: 0, X.X86_REG_ECX: 1, X.X86_REG_EDX: 2, X.X86_REG_EBX: 3,
             X.X86_REG_ESP: 4, X.X86_REG_EBP: 5, X.X86_REG_ESI: 6, X.X86_REG_EDI: 7}
FLAG_BITS = {"cf": 0, "pf": 2, "zf": 6, "sf": 7, "of": 11}
ALL_FLAG_MASK = sum(1 << b for b in FLAG_BITS.values())

SKIP = {"call", "ret", "jmp", "loop", "loope", "loopne", "jecxz", "int3", "cpuid", "nop", "wait", "fwait",
        "leave", "pushfd", "popfd", "rdtsc", "hlt", "int", "int1", "into", "iretd", "retf", "ljmp", "lcall"}
STRING = {"movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsw", "lodsd", "scasb", "cmpsb"}
FPU_PROLOGUE = bytes([0xDB, 0xE3]) + b"".join(b"\xDD\x05" + struct.pack("<I", FPU_IN + 8 * k) for k in range(4))
FPU_DEPTH = {"fld": 1, "fild": 1, "fld1": 1, "fldz": 1, "fldpi": 1, "fldl2e": 1, "fldln2": 1, "fldlg2": 1, "fldl2t": 1,
             "fptan": 1, "fsincos": 1, "fstp": -1, "fistp": -1, "fbstp": -1, "fcomp": -1, "fucomp": -1, "ficomp": -1,
             "fcompp": -2, "fucompp": -2, "faddp": -1, "fsubp": -1, "fsubrp": -1, "fmulp": -1, "fdivp": -1, "fdivrp": -1,
             "fyl2x": -1, "fpatan": -1, "fninit": -4}


def fpu_epilogue(ins):
    """Store only the stack slots that are valid after the instruction (the prologue loads four)."""
    depth = 4 + FPU_DEPTH.get(ins.mnemonic, 0)
    return b"".join(b"\xDD\x1D" + struct.pack("<I", FPU_OUT + 8 * k) for k in range(max(0, min(depth, 4))))


def undefined_flags(ins, regs_in):
    """Flags the architecture leaves undefined for this instruction and input."""
    m = ins.mnemonic.replace("lock ", "")
    if m in ("mul", "imul"):
        return {"sf", "zf", "pf"}
    if m in ("div", "idiv"):
        return set(FLAG_BITS)
    if m in ("bsf", "bsr"):
        return {"cf", "of", "sf", "pf"}
    if m in ("bt", "bts", "btr", "btc"):
        return {"of", "sf", "pf", "zf"}
    if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr", "shld", "shrd"):
        if ins.operands[-1].type == X.X86_OP_IMM:
            n = ins.operands[-1].imm & 31
        else:
            n = regs_in[1] & 31
        out = set()
        if n != 1:
            out.add("of")
        if m in ("shld", "shrd"):
            out |= {"pf", "af"}
            if n > ins.operands[0].size * 8:
                out |= set(FLAG_BITS)
        if m in ("shl", "sal", "shr", "sar") and n > ins.operands[0].size * 8:
            out.add("cf")
        return out
    return set()


def shape_key(ins):
    parts = [ins.mnemonic]
    for op in ins.operands:
        if op.type == X.X86_OP_REG:
            n = ins.reg_name(op.reg)
            parts.append(f"r{op.size}" + ("cl" if n == "cl" else "h" if n in ("ah", "bh", "ch", "dh") else ""))
        elif op.type == X.X86_OP_IMM:
            parts.append("i1" if op.imm == 1 else f"i{op.size}")
        else:
            parts.append(f"m{op.size}" + ("fs" if op.mem.segment == X.X86_REG_FS else "") +
                         ("abs" if not op.mem.base and not op.mem.index else ""))
    return " ".join(parts)


def collect_shapes(L, fns, per_shape):
    shapes = defaultdict(list)
    for _, _, rs in fns:
        for lo, hi in rs:
            for ins in L.disasm_range(lo, hi):
                m = ins.mnemonic
                if m in SKIP or m.startswith("j") or m.startswith("set") and False:
                    continue
                if any(op.type == X.X86_OP_MEM and (op.mem.segment == X.X86_REG_FS or (not op.mem.base and not op.mem.index))
                       for op in ins.operands):
                    continue
                if m in ("push", "pop") and any(op.type == X.X86_OP_MEM for op in ins.operands):
                    pass
                key = shape_key(ins)
                if len(shapes[key]) < per_shape:
                    shapes[key].append(bytes(ins.bytes))
    return shapes


def is_fpu(ins):
    return ins.mnemonic.startswith("f")


def lift_snippet(L, code, name):
    """Lift raw instruction bytes (placed at CODE_AT) into one C test function."""
    md = L.md
    lines = [f"static void {name}(CPU* c) {{",
             "  uint32_t tgt = 0, t, a, b, r, ret = 0; uint64_t q; int ind = 0;",
             "  (void)tgt; (void)t; (void)a; (void)b; (void)r; (void)q; (void)ind; (void)ret;"]
    g = lift.FnGen.__new__(lift.FnGen)
    g.L, g.entry, g.addrs, g.out, g.labelset = L, CODE_AT, set(), [], set()
    for ins in md.disasm(code, CODE_AT):
        g.lift(ins, lift.ALL_FLAGS)
    lines += g.out
    lines.append("}")
    return "\n".join(lines)


def make_state(rng, ins):
    regs = [rng.getrandbits(32) for _ in range(8)]
    bases, indexes = set(), set()
    for op in ins.operands:
        if op.type == X.X86_OP_MEM:
            if op.mem.base in REG_INDEX:
                bases.add(REG_INDEX[op.mem.base])
            if op.mem.index in REG_INDEX:
                indexes.add(REG_INDEX[op.mem.index])
    for k in bases:
        regs[k] = SANDBOX + 0x400 + rng.randrange(0, 0x200)
    for k in indexes:
        regs[k] = rng.randrange(0, 0x40)
    regs[4] = SANDBOX + 0x800
    m = ins.mnemonic
    if m in STRING or m.startswith("rep"):
        regs[6] = SANDBOX + 0x200 + rng.randrange(0, 0x100)
        regs[7] = SANDBOX + 0x600 + rng.randrange(0, 0x100)
        regs[1] = rng.randrange(0, 24)
        if rng.random() < 0.5:
            regs[0] = (regs[0] & ~0xFF) | rng.choice([0, 0x41, 0x61])
    if m in ("div", "idiv"):
        regs[2] = rng.randrange(0, 4)
    if m.replace("lock ", "") in ("shl", "shr", "sar", "rol", "ror", "rcl", "rcr", "shld", "shrd", "sal") and rng.random() < 0.3:
        regs[1] = (regs[1] & ~0xFF) | rng.choice([0, 1, 31, 32, 33])
    flags = 0x202 | sum(rng.getrandbits(1) << b for b in FLAG_BITS.values())
    seed = rng.getrandbits(32) | 1
    pool = [0.0, 1.0, -2.5, 1e10, 123456789.0, 3.5, -0.5, 2.0 ** 40 + 3, 1.0 / 3, -7.25, 1e-5, 42.0, 0.75, 0.25, -3.0]
    fpu = tuple(rng.choice(pool) for _ in range(4))
    return regs, flags, seed, fpu


def xorshift_fill(seed):
    s, words = seed, []
    for _ in range(SANDBOX_SIZE // 4):
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        words.append(s)
    return bytearray(struct.pack(f"<{len(words)}I", *words))


def run_unicorn(code, regs, flags, seed, fpu, fpu_test):
    mem = xorshift_fill(seed)
    if fpu_test:
        mem[0xF00:0xF20] = struct.pack("<4d", *fpu)
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(SANDBOX, SANDBOX_SIZE)
    uc.mem_map(CODE_AT, 0x1000)
    uc.mem_write(SANDBOX, bytes(mem))
    uc.mem_write(CODE_AT, code)
    for r, v in zip(REGS, regs):
        uc.reg_write(r, v)
    uc.reg_write(U.UC_X86_REG_EFLAGS, flags)
    status = 0
    try:
        uc.emu_start(CODE_AT, CODE_AT + len(code), count=64)
    except UcError:
        status = 1
    out_regs = [uc.reg_read(r) for r in REGS]
    return status, out_regs, uc.reg_read(U.UC_X86_REG_EFLAGS), bytes(uc.mem_read(SANDBOX, SANDBOX_SIZE))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("functions")
    ap.add_argument("work")
    ap.add_argument("--cases", type=int, default=24)
    ap.add_argument("--per-shape", type=int, default=2)
    ap.add_argument("--only")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)

    L = lift.Lifter(a.exe)
    fns = []
    for line in open(a.functions):
        e, n, rs = line.split()
        fns.append((int(e, 16), n, [tuple(int(x, 16) for x in r.split("-")) for r in rs.split(",")]))
    shapes = collect_shapes(L, fns, a.per_shape)
    if a.only:
        shapes = {k: v for k, v in shapes.items() if k.split()[0] == a.only}
    print(f"{len(shapes)} instruction shapes")

    tests = []
    md = L.md
    for key, samples in sorted(shapes.items()):
        for raw in samples:
            ins = next(md.disasm(raw, CODE_AT))
            fpu_test = is_fpu(ins)
            code = (FPU_PROLOGUE + raw + fpu_epilogue(ins)) if fpu_test else raw
            tests.append((key, ins, code, fpu_test))

    src = ['#include "rt.h"', ""]
    for k, (key, ins, code, fpu_test) in enumerate(tests):
        try:
            src.append(f"/* {key}: {ins.mnemonic} {ins.op_str} */")
            src.append(lift_snippet(L, code, f"t_{k}"))
        except Exception as ex:  # lifter refused: the test fails loudly at run time
            src.append(f"static void t_{k}(CPU* c) {{ rt_unimpl(c, 0, \"{ins.mnemonic}\"); }} /* {ex} */")
    src.append("typedef void (*TestFn)(CPU*);")
    src.append("const TestFn difftests[] = {" + ", ".join(f"t_{k}" for k in range(len(tests))) + "};")
    src.append(f"const uint32_t difftest_count = {len(tests)};")
    open(os.path.join(a.work, "tests.c"), "w").write("\n".join(src) + "\n")

    here = os.path.dirname(os.path.abspath(__file__))
    runtime = os.path.join(here, "..", "runtime")
    vcvars = r"C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars32.bat"
    exe = os.path.join(a.work, "harness.exe")
    bat = os.path.join(a.work, "build.bat")
    with open(bat, "w") as f:
        f.write(f'@call "{vcvars}" >nul 2>nul\r\n'
                f'cl /nologo /Od /W0 /GS- /I"{runtime}" /Fe"{exe}" /Fo"{a.work}\\\\" '
                f'"{os.path.join(a.work, "tests.c")}" "{os.path.join(here, "harness.c")}" "{os.path.join(runtime, "rt.c")}"\r\n')
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-4000:], r.stderr[-2000:])
        sys.exit(1)

    rng = random.Random(1234)
    cases = []
    with open(os.path.join(a.work, "in.bin"), "wb") as f:
        for k, (key, ins, code, fpu_test) in enumerate(tests):
            for _ in range(a.cases):
                regs, flags, seed, fpu = make_state(rng, ins)
                cases.append((k, regs, flags, seed, fpu))
                f.write(struct.pack("<I8III", k, *regs, flags, seed))
                f.write(struct.pack("<8d", *fpu, 0, 0, 0, 0))
                f.write(struct.pack("<III", 0, 0x37F, 1 if fpu_test else 0))
    r = subprocess.run([exe, os.path.join(a.work, "in.bin"), os.path.join(a.work, "out.bin")], capture_output=True, text=True)
    if r.returncode:
        print("harness failed", r.returncode, r.stderr)
        sys.exit(1)

    rec = 4 + 32 + 4 + SANDBOX_SIZE
    data = open(os.path.join(a.work, "out.bin"), "rb").read()
    fails = defaultdict(list)
    passed = defaultdict(int)
    for n, (k, regs, flags, seed, fpu) in enumerate(cases):
        key, ins, code, fpu_test = tests[k]
        chunk = data[n * rec:(n + 1) * rec]
        status = struct.unpack_from("<I", chunk, 0)[0]
        got_regs = list(struct.unpack_from("<8I", chunk, 4))
        got_flags = struct.unpack_from("<I", chunk, 36)[0]
        got_mem = chunk[40:40 + SANDBOX_SIZE]
        u_status, u_regs, u_flags, u_mem = run_unicorn(code, regs, flags, seed, fpu, fpu_test)
        problems = []
        if status != u_status:
            problems.append(f"status lifted={status} unicorn={u_status}")
        elif status == 0:
            for i, (x, y) in enumerate(zip(got_regs, u_regs)):
                if x != y:
                    problems.append(f"{['eax','ecx','edx','ebx','esp','ebp','esi','edi'][i]} lifted={x:08x} unicorn={y:08x}")
            mask = ALL_FLAG_MASK
            for fl in undefined_flags(ins, regs):
                mask &= ~(1 << FLAG_BITS.get(fl, 4))
            if (got_flags ^ u_flags) & mask:
                diff = [fl for fl, b in FLAG_BITS.items() if ((got_flags ^ u_flags) & mask) >> b & 1]
                problems.append(f"flags {','.join(diff)} lifted={got_flags:04x} unicorn={u_flags:04x}")
            if got_mem != u_mem:
                if fpu_test and got_mem[:0xF80] == u_mem[:0xF80]:
                    g = struct.unpack_from("<4d", got_mem, 0xF80)
                    u = struct.unpack_from("<4d", u_mem, 0xF80)
                    if any(not (x == y or (x != x and y != y) or abs(x - y) <= 1e-12 * max(abs(x), abs(y))) for x, y in zip(g, u)):
                        problems.append(f"fpu out lifted={g} unicorn={u}")
                else:
                    off = next(i for i in range(SANDBOX_SIZE) if got_mem[i] != u_mem[i])
                    problems.append(f"memory differs at +{off:03x}: lifted={got_mem[off:off+8].hex()} unicorn={u_mem[off:off+8].hex()}")
        if problems:
            fails[key].append((ins, regs, flags, problems))
        else:
            passed[key] += 1

    total_shapes = len({t[0] for t in tests})
    print(f"{total_shapes - len(fails)}/{total_shapes} shapes pass ({sum(passed.values())} cases passed)")
    with open(os.path.join(a.work, "report.txt"), "w") as rep:
        for key in sorted(fails, key=lambda k: -len(fails[k])):
            ins, regs, flags, problems = fails[key][0]
            line = (f"FAIL {key} ({len(fails[key])} cases) e.g. {ins.mnemonic} {ins.op_str} "
                    f"regs={' '.join(f'{v:08x}' for v in regs)} flags={flags:04x}: {'; '.join(problems)}")
            rep.write(line + "\n")
            print(line)


if __name__ == "__main__":
    main()
