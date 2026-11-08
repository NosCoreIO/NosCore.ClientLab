"""Instruction forms the lifter supports, assembled with GNU as, so difftest.py can check them without the client.

Each entry is one test: Intel-syntax lines joined by ';', assembled into one snippet. Memory operands only use
registers as bases or indexes (difftest.py points those into the sandbox). A snippet may follow its instruction with
another one that exposes state the harness does not compare directly (fnstsw ax after an x87 compare).
"""
import os
import subprocess
import tempfile

CC = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"]
M8, M16, M32 = "byte ptr [ebx+3]", "word ptr [esi+6]", "dword ptr [edi+ecx*4+8]"


def forms():
    t = []
    for op in ("add", "sub", "adc", "sbb", "cmp", "and", "or", "xor", "test"):
        t += [f"{op} al, bl", f"{op} ah, dl", f"{op} cl, 0x80", f"{op} dx, si", f"{op} di, 0x7fff",
              f"{op} ebx, eax", f"{op} edx, 0x80000000", f"{op} eax, 0x7f", f"{op} {M8}, al", f"{op} {M16}, cx",
              f"{op} {M32}, edx", f"{op} edx, {M32}", f"{op} {M32}, -3", f"{op} {M16}, 0x1234", f"{op} {M8}, 0xfe"]
    for op in ("inc", "dec", "neg", "not"):
        t += [f"{op} al", f"{op} bh", f"{op} cx", f"{op} edx", f"{op} {M8}", f"{op} {M16}", f"{op} {M32}"]
    for op in ("shl", "shr", "sar", "rol", "ror", "rcl", "rcr"):
        for dst in ("al", "dh", "cx", "edx", M8, M16, M32):
            t += [f"{op} {dst}, 1", f"{op} {dst}, cl", f"{op} {dst}, 0", f"{op} {dst}, 5"]
        t += [f"{op} edx, 31", f"{op} bl, 9", f"{op} si, 17"]
    for op in ("shld", "shrd"):
        t += [f"{op} eax, edx, 7", f"{op} eax, edx, cl", f"{op} {M32}, ebx, 1", f"{op} edx, eax, 0",
              f"{op} {M32}, ebx, cl", f"{op} dx, ax, 5", f"{op} dx, ax, cl"]
    for op in ("mul", "imul", "div", "idiv"):
        t += [f"{op} bl", f"{op} ch", f"{op} si", f"{op} ebx", f"{op} {M8}", f"{op} {M16}", f"{op} {M32}"]
    t += ["imul ecx, ebx", "imul dx, si", f"imul eax, {M32}", "imul ecx, ebx, 0x12345", "imul dx, si, -7",
          f"imul eax, {M32}, 100", "imul esi, esi, 3"]
    for op in ("bt", "bts", "btr", "btc"):
        t += [f"{op} eax, ebx", f"{op} eax, 17", f"{op} {M32}, 5", f"{op} dx, si", f"{op} {M16}, 3",
              f"{op} dword ptr [ebx+0x40], ecx", f"{op} word ptr [ebx+0x40], si"]
    for op in ("bsf", "bsr"):
        t += [f"{op} eax, ebx", f"{op} dx, si", f"{op} ecx, {M32}"]
    for cc in CC:
        t += [f"set{cc} al", f"set{cc} {M8}"]
    t += ["movzx eax, bl", "movzx edx, ah", "movzx ecx, si", f"movzx eax, {M8}", f"movzx edx, {M16}", "movzx cx, bl",
          "movsx eax, bl", "movsx edx, ah", "movsx ecx, si", f"movsx eax, {M8}", f"movsx edx, {M16}", "movsx cx, bl",
          "xchg al, bh", "xchg cx, dx", "xchg eax, esi", f"xchg {M32}, ecx", f"xchg {M8}, dl", "bswap edx",
          "cbw", "cwde", "cwd", "cdq", "lahf", "sahf", "cmc", "clc", "stc", "lea eax, [ebx+ecx*8-5]",
          "lea dx, [esi+edi]", "mov ah, dl", f"mov {M16}, 0xbeef", f"mov {M8}, ch",
          "push ax", "push dword ptr [ebx+4]", "pop edx", "pop ax", "pop dword ptr [esi+4]", "pop dword ptr [esp+4]",
          "lock cmpxchg dword ptr [ebx+8], ecx", "cmpxchg dword ptr [ebx+8], ecx", "lock cmpxchg byte ptr [ebx+3], dl",
          "lock xadd dword ptr [ebx+8], ecx", "lock inc dword ptr [ebx+8]", "lock dec dword ptr [ebx+8]",
          "movq mm1, qword ptr [esi+8]; movq qword ptr [edi], mm1"]
    t += ["movsb", "movsw", "movsd", "rep movsb", "rep movsw", "rep movsd", "stosb", "stosw", "stosd", "rep stosb",
          "rep stosw", "rep stosd", "lodsb", "lodsw", "lodsd", "scasb", "repne scasb", "repne scasw", "repe scasb",
          "cmpsb", "repe cmpsb", "repe cmpsw", "repe cmpsd"]
    fx = "fnstsw ax"
    for m in ("dword ptr [ebx+8]", "qword ptr [ebx+8]"):
        t += [f"fld {m}", f"fst {m}", f"fstp {m}"]
        for op in ("fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr"):
            t.append(f"{op} {m}")
        t += [f"fcom {m}; {fx}", f"fcomp {m}; {fx}"]
    t += ["fld tbyte ptr [ebx+8]", "fstp tbyte ptr [ebx+8]", "fbstp tbyte ptr [ebx+8]"]
    for m in ("word ptr [ebx+8]", "dword ptr [ebx+8]", "qword ptr [ebx+8]"):
        t += [f"fild {m}", f"fistp {m}"]
    for m in ("word ptr [ebx+8]", "dword ptr [ebx+8]"):
        t.append(f"fist {m}")
        for op in ("fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr"):
            t.append(f"{op} {m}")
        t += [f"ficom {m}; {fx}", f"ficomp {m}; {fx}"]
    for op in ("fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr"):
        t += [f"{op} st, st(2)", f"{op} st(3), st", f"{op}p st(2), st", f"{op}p"]
    t += ["fcom st(2); " + fx, "fcomp st(1); " + fx, "fcompp; " + fx, "fucom st(3); " + fx, "fucomp st(2); " + fx,
          "fucompp; " + fx, "ftst; " + fx, "fcomp st(1); fnstsw ax; sahf",
          "fxch st(2)", "fxch", "fld st(2)", "fst st(3)", "fstp st(2)", "fstp st(0)", "fchs", "fabs", "fsqrt",
          "frndint", "fscale", "fprem", "fsin", "fcos", "fptan", "fsincos", "fpatan", "fyl2x", "f2xm1", "fld1",
          "fldz", "fldpi", "fldl2e", "fldln2", "fldlg2", "fldl2t", "fnclex", "fninit",
          "fnstcw word ptr [ebx+8]", "fnstcw word ptr [ebx+8]; or word ptr [ebx+8], 0xc00; fldcw word ptr [ebx+8]; frndint",
          "fnstcw word ptr [ebx+8]; or word ptr [ebx+8], 0xc00; fldcw word ptr [ebx+8]; fistp dword ptr [ebx+16]",
          "fnstcw word ptr [ebx+8]; or word ptr [ebx+8], 0x400; fldcw word ptr [ebx+8]; fistp qword ptr [ebx+16]",
          "fstsw word ptr [ebx+8]"]
    return t


def assemble(lines, work):
    """Returns the machine code of each test, assembled with GNU as for i386."""
    out = []
    with tempfile.TemporaryDirectory(dir=work) as tmp:
        for k, text in enumerate(lines):
            s, o, b = (os.path.join(tmp, f"t{k}.{e}") for e in ("s", "o", "bin"))
            with open(s, "w") as f:
                f.write(".intel_syntax noprefix\n" + text.replace(";", "\n") + "\n")
            subprocess.run(["as", "--32", s, "-o", o], check=True)
            subprocess.run(["objcopy", "-O", "binary", "-j", ".text", o, b], check=True)
            out.append(open(b, "rb").read())
    return out
