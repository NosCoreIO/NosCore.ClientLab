"""Static recompiler: lifts the client's x86 code to C, one C function per Ghidra function.

Generated code is derived from the user's own client and is written outside the repo.
"""
import argparse
import bisect
import io
import os
import struct
from collections import defaultdict

import capstone
from capstone import x86_const as X
import pefile

ALL_FLAGS = frozenset("cf zf sf of pf".split())

REG32 = {"eax": 0, "ecx": 1, "edx": 2, "ebx": 3, "esp": 4, "ebp": 5, "esi": 6, "edi": 7}
REG16 = {"ax": 0, "cx": 1, "dx": 2, "bx": 3, "sp": 4, "bp": 5, "si": 6, "di": 7}
REG8L = {"al": 0, "cl": 1, "dl": 2, "bl": 3}
REG8H = {"ah": 0, "ch": 1, "dh": 2, "bh": 3}

UTYPE = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
STYPE = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}
MSB = {1: "0x80u", 2: "0x8000u", 4: "0x80000000u"}

JCC = {
    "je": "ZF", "jne": "!ZF", "jb": "CF", "jae": "!CF", "jbe": "(CF||ZF)", "ja": "(!CF&&!ZF)",
    "jl": "(SF!=OF)", "jge": "(SF==OF)", "jle": "(ZF||SF!=OF)", "jg": "(!ZF&&SF==OF)",
    "js": "SF", "jns": "!SF", "jo": "OF", "jno": "!OF", "jp": "PF", "jnp": "!PF",
}
JCC_READS = {
    "je": {"zf"}, "jne": {"zf"}, "jb": {"cf"}, "jae": {"cf"}, "jbe": {"cf", "zf"}, "ja": {"cf", "zf"},
    "jl": {"sf", "of"}, "jge": {"sf", "of"}, "jle": {"zf", "sf", "of"}, "jg": {"zf", "sf", "of"},
    "js": {"sf"}, "jns": {"sf"}, "jo": {"of"}, "jno": {"of"}, "jp": {"pf"}, "jnp": {"pf"},
}
SETCC = {"set" + k[1:]: v for k, v in JCC.items()}
SETCC_READS = {"set" + k[1:]: v for k, v in JCC_READS.items()}

ARITH_W = {"add", "sub", "cmp", "adc", "sbb", "neg", "and", "or", "xor", "test", "shl", "sal", "shr", "sar",
           "imul", "mul", "bsf", "bsr", "shld", "shrd", "cmpxchg", "xadd"}
FLAG_RW = {
    "adc": ({"cf"}, ALL_FLAGS), "sbb": ({"cf"}, ALL_FLAGS),
    "inc": (set(), {"zf", "sf", "of", "pf"}), "dec": (set(), {"zf", "sf", "of", "pf"}),
    "bt": (set(), {"cf"}), "bts": (set(), {"cf"}), "btr": (set(), {"cf"}), "btc": (set(), {"cf"}),
    "sahf": (set(), ALL_FLAGS), "popfd": (set(), ALL_FLAGS), "pushfd": (ALL_FLAGS, set()),
    "cmc": ({"cf"}, {"cf"}), "clc": (set(), {"cf"}), "stc": (set(), {"cf"}),
    "rcl": ({"cf"}, {"cf", "of"}), "rcr": ({"cf"}, {"cf", "of"}), "rol": (set(), {"cf", "of"}),
    "ror": (set(), {"cf", "of"}),
    "repe cmpsb": (ALL_FLAGS, ALL_FLAGS), "repe cmpsw": (ALL_FLAGS, ALL_FLAGS), "repe cmpsd": (ALL_FLAGS, ALL_FLAGS),
    "repne scasb": (ALL_FLAGS, ALL_FLAGS), "repne scasw": (ALL_FLAGS, ALL_FLAGS), "repe scasb": (ALL_FLAGS, ALL_FLAGS),
    "fcomi": (set(), ALL_FLAGS), "fucomi": (set(), ALL_FLAGS), "fcomip": (set(), ALL_FLAGS),
    "fucomip": (set(), ALL_FLAGS),
}


SHIFTS = {"shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr", "shld", "shrd"}


def flag_rw_ins(i):
    """Like flag_rw, but a shift by cl may shift by 0 and leave every flag as it was."""
    r, w = flag_rw(i.mnemonic)
    if i.mnemonic in SHIFTS and i.operands and i.operands[-1].type == X.X86_OP_REG:
        r = set(r) | ALL_FLAGS
    return r, w


def flag_rw(m):
    if m in JCC_READS:
        return JCC_READS[m], set()
    if m in SETCC_READS:
        return SETCC_READS[m], set()
    if m in FLAG_RW:
        r, w = FLAG_RW[m]
        return set(r), set(w)
    if m in ARITH_W:
        return set(), set(ALL_FLAGS)
    return set(), set()


class Lifter:
    def __init__(self, exe):
        self.pe = pefile.PE(exe)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        code = self.pe.sections[0]
        self.code_lo = self.base + code.VirtualAddress
        self.code = code.get_data()[: code.Misc_VirtualSize]
        self.code_hi = self.code_lo + len(self.code)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.reloc_targets = self._reloc_code_targets()
        self.reloc_slots = self._reloc_slots()

    def _reloc_code_targets(self):
        out = set()
        for blk in self.pe.DIRECTORY_ENTRY_BASERELOC:
            for e in blk.entries:
                if e.type != 3:
                    continue
                va = e.rva + self.base
                val = struct.unpack("<I", self.pe.get_data(e.rva, 4))[0]
                if self.code_lo <= val < self.code_hi:
                    out.add(val)
        return out

    def _reloc_slots(self):
        out = {}
        for blk in self.pe.DIRECTORY_ENTRY_BASERELOC:
            for e in blk.entries:
                if e.type == 3:
                    out[e.rva + self.base] = struct.unpack("<I", self.pe.get_data(e.rva, 4))[0]
        return out

    def jump_table(self, ins):
        """Targets of 'jmp dword ptr [reg*4 + table]', read through relocated table slots."""
        if ins.mnemonic != "jmp" or not ins.operands or ins.operands[0].type != X.X86_OP_MEM:
            return []
        m = ins.operands[0].mem
        if m.base or not m.index or m.scale != 4:
            return []
        out, slot = [], m.disp & 0xFFFFFFFF
        while slot in self.reloc_slots and len(out) < 1024:
            v = self.reloc_slots[slot]
            if not (self.code_lo <= v < self.code_hi):
                break
            out.append(v)
            slot += 4
        return out

    def disasm_range(self, lo, hi):
        out = []
        pos = lo
        while pos < hi:
            off = pos - self.code_lo
            ins = next(self.md.disasm(self.code[off: off + 16], pos), None)
            if ins is None:
                break
            out.append(ins)
            pos += ins.size
        return out


def is_code(lifter, a):
    return lifter.code_lo <= a < lifter.code_hi


class FnGen:
    def __init__(self, lifter, entry, name, ranges, label_index):
        self.L = lifter
        self.entry = entry
        self.name = name
        self.ranges = ranges
        self.label_index = label_index
        self.ins = []
        for lo, hi in ranges:
            self.ins.extend(lifter.disasm_range(lo, hi))
        self.addrs = {i.address for i in self.ins}
        if label_index is not None:
            self._absorb_orphans(label_index)
        self.ins.sort(key=lambda i: i.address)
        self.out = []
        self.uses_seh = any(self._writes_fs_esp(i) for i in self.ins)

    def _absorb_orphans(self, covered):
        """Pull in code this function reaches (pushed return labels, jump targets) that no function owns,
        e.g. Delphi try/finally epilogues that Ghidra cut off after the 'push label; ...; ret'."""
        L = self.L
        changed = True
        while changed:
            changed = False
            roots = []
            self.ins.sort(key=lambda i: i.address)
            for k, i in enumerate(self.ins):
                m = i.mnemonic
                for t in L.jump_table(i):
                    if t not in self.addrs:
                        roots.append((t, True))
                if not i.operands or i.operands[0].type != X.X86_OP_IMM:
                    continue
                if m == "push" and not self._is_code_push(k):
                    continue
                if m == "push" or m.startswith("j") or m.startswith("loop"):
                    t = i.operands[0].imm & 0xFFFFFFFF
                    if L.code_lo <= t < L.code_hi and t not in self.addrs and (m == "push" or not covered(t)):
                        roots.append((t, m == "push"))
            for pos, force in list(roots):
                first = L.disasm_range(pos, pos + 1)
                if force and first and first[0].mnemonic == "jmp":
                    # Delphi SEH handler: 'jmp @HandleFinally; jmp @finally' - _HandleFinally calls the second jump.
                    roots.append((pos + first[0].size, True))
            for pos, force in roots:
                work = [(pos, force)]
                while work:
                    p, force = work.pop()
                    while L.code_lo <= p < L.code_hi and p not in self.addrs and (force or not covered(p)):
                        dec = L.disasm_range(p, p + 1)
                        if not dec:
                            break
                        i = dec[0]
                        self.ins.append(i)
                        self.addrs.add(p)
                        changed = True
                        m = i.mnemonic
                        if (m.startswith("j") or m.startswith("loop")) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                            work.append((i.operands[0].imm & 0xFFFFFFFF, False))
                        work.extend((t, True) for t in L.jump_table(i))
                        if m in ("jmp", "ret", "int3"):
                            break
                        p = i.address + i.size

    def _is_code_push(self, k):
        """Delphi pushes code addresses in two shapes: the finally return label right after the
        'mov fs:[reg], reg' that ends a try, and the SEH handler right before 'push fs:[..]'."""
        prev = self.ins[k - 1] if k > 0 else None
        nxt = self.ins[k + 1] if k + 1 < len(self.ins) else None
        if prev is not None and prev.mnemonic == "mov" and prev.operands and prev.operands[0].type == X.X86_OP_MEM \
                and prev.operands[0].mem.segment == X.X86_REG_FS:
            return True
        if nxt is not None and nxt.mnemonic == "push" and nxt.operands and nxt.operands[0].type == X.X86_OP_MEM \
                and nxt.operands[0].mem.segment == X.X86_REG_FS:
            return True
        return False

    @staticmethod
    def _writes_fs_esp(ins):
        if ins.mnemonic != "mov" or len(ins.operands) != 2:
            return False
        d, s = ins.operands
        return d.type == X.X86_OP_MEM and d.mem.segment == X.X86_REG_FS and s.type == X.X86_OP_REG and s.reg == X.X86_REG_ESP

    def inside(self, a):
        return a in self.addrs

    def labels(self):
        labs = {self.entry} if self.entry in self.addrs else set()
        labs |= {x for x in getattr(self, "extra_labels", ()) if x in self.addrs}
        for idx, i in enumerate(self.ins):
            m = i.mnemonic
            if (m.startswith("j") or m == "loop" or m == "call") and i.operands and i.operands[0].type == X.X86_OP_IMM:
                t = i.operands[0].imm & 0xFFFFFFFF
                if self.inside(t):
                    labs.add(t)
            if m == "call" or m.startswith("j") or m in ("ret", "loop"):
                nxt = i.address + i.size
                if self.inside(nxt):
                    labs.add(nxt)
            if m == "push" and i.operands and i.operands[0].type == X.X86_OP_IMM:
                t = i.operands[0].imm & 0xFFFFFFFF
                if self.inside(t):
                    labs.add(t)
            if i.address in self.L.reloc_targets:
                labs.add(i.address)
        for idx in range(1, len(self.ins)):
            prev, cur = self.ins[idx - 1], self.ins[idx]
            if prev.address + prev.size != cur.address:
                labs.add(cur.address)
        return labs

    # ---- liveness ----
    def flag_liveness(self, labels):
        blocks = []
        cur = []
        for i in self.ins:
            if i.address in labels and cur:
                blocks.append(cur)
                cur = []
            cur.append(i)
            m = i.mnemonic
            if m.startswith("j") or m in ("ret", "call", "loop") or m.startswith("rep"):
                blocks.append(cur)
                cur = []
        if cur:
            blocks.append(cur)
        start_of = {b[0].address: k for k, b in enumerate(blocks)}
        succ = []
        for k, b in enumerate(blocks):
            last = b[-1]
            m = last.mnemonic
            s = set()
            exits = False
            nxt = last.address + last.size
            target = None
            if (m.startswith("j") or m == "loop") and last.operands and last.operands[0].type == X.X86_OP_IMM:
                target = last.operands[0].imm & 0xFFFFFFFF
            if m == "jmp":
                if target is not None and target in start_of:
                    s.add(start_of[target])
                else:
                    exits = True
            elif m.startswith("j") or m == "loop":
                if target is not None and target in start_of:
                    s.add(start_of[target])
                else:
                    exits = True
                if nxt in start_of:
                    s.add(start_of[nxt])
                else:
                    exits = True
            elif m == "ret":
                exits = True
            else:
                if nxt in start_of:
                    s.add(start_of[nxt])
                else:
                    exits = True
                if m == "call":
                    exits = True
            succ.append((s, exits))
        live_in = [set(ALL_FLAGS) for _ in blocks]
        changed = True
        while changed:
            changed = False
            for k in range(len(blocks) - 1, -1, -1):
                s, exits = succ[k]
                live = set(ALL_FLAGS) if exits else set()
                for t in s:
                    live |= live_in[t]
                for i in reversed(blocks[k]):
                    r, w = flag_rw_ins(i)
                    if i.mnemonic in ("call",):
                        r, w = set(ALL_FLAGS), set(ALL_FLAGS)
                    live = (live - w) | r
                if live != live_in[k]:
                    live_in[k] = live
                    changed = True
        live_after = {}
        for k, b in enumerate(blocks):
            s, exits = succ[k]
            live = set(ALL_FLAGS) if exits else set()
            for t in s:
                live |= live_in[t]
            for i in reversed(b):
                live_after[i.address] = set(live)
                r, w = flag_rw_ins(i)
                if i.mnemonic == "call":
                    r, w = set(ALL_FLAGS), set(ALL_FLAGS)
                live = (live - w) | r
        return live_after

    # ---- operands ----
    def reg(self, r, size=None):
        n = self.L.md.reg_name(r)
        if n in REG32:
            return f"R32({REG32[n]})"
        if n in REG16:
            return f"R16({REG16[n]})"
        if n in REG8L:
            return f"R8L({REG8L[n]})"
        if n in REG8H:
            return f"R8H({REG8H[n]})"
        if n.startswith("mm"):
            return f"c->mm[{n[2:]}]"
        raise NotImplementedError(f"reg {n}")

    def addr(self, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg(m.base))
        if m.index:
            idx = self.reg(m.index)
            parts.append(f"{idx}*{m.scale}" if m.scale != 1 else idx)
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xFFFFFFFF:x}u")
        return "(uint32_t)(" + "+".join(parts) + ")"

    def mem(self, op, size=None):
        size = size or op.size
        a = self.addr(op)
        if op.mem.segment == X.X86_REG_FS and not op.mem.base and not op.mem.index and op.mem.disp == 0x2C:
            return "rt_tls_array(c)"
        if op.mem.segment == X.X86_REG_FS:
            return f"FS{size * 8}({a})"
        return f"M{size * 8}({a})"

    def rd(self, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg(op.reg)
        if op.type == X.X86_OP_IMM:
            return f"(({UTYPE[size]})0x{op.imm & ((1 << (size * 8)) - 1):x}u)"
        return self.mem(op, size)

    def wr(self, op, val):
        if op.type == X.X86_OP_REG:
            return f"{self.reg(op.reg)} = {val};"
        if op.mem.segment == X.X86_REG_FS:
            return f"FS{op.size * 8}_W({self.addr(op)}, {val});"
        return f"{self.mem(op)} = {val};"

    # ---- emit helpers ----
    def e(self, s):
        self.out.append("  " + s)

    def flags_res(self, live, r, size):
        ut = UTYPE[size]
        if "zf" in live:
            self.e(f"ZF = ({ut}){r} == 0;")
        if "sf" in live:
            self.e(f"SF = (({ut}){r} & {MSB[size]}) != 0;")
        if "pf" in live:
            self.e(f"PF = PARITY((uint8_t){r});")

    def jump_to(self, t, ret_expr="ret"):
        if self.inside(t):
            return f"goto L_{t:08x};"
        return f"{{ rt_jump(c, 0x{t:08x}u, {ret_expr}); EXIT; }}"

    def gen(self):
        labels = self.labels()
        self.labelset = labels
        live_after = self.flag_liveness(labels)
        fn = f"f_{self.entry:08x}"
        o = self.out
        o.append(f"/* {self.name} */")
        o.append(f"void {fn}(CPU* c, uint32_t entry, uint32_t ret) {{")
        o.append("  uint32_t tgt = entry, t, a, b, r; uint64_t q; int ind = 0; (void)t; (void)a; (void)b; (void)r; (void)q;")
        o.append("  RT_TRACE(c, entry, ret);")
        if self.uses_seh:
            o.append("  RtAct act; act.fn = " + fn + "; act.guest_esp = c->esp; act.host_esp = c->host_esp;")
            o.append("  if (rt_setjmp(act.jb)) { tgt = c->lj_target; ind = 0; rt_act_push(c, &act); goto dispatch; }")
            o.append("  rt_act_push(c, &act);")
            o.append("#define EXIT do { rt_act_pop(c, &act); return; } while (0)")
        else:
            o.append("#define EXIT return")
        o.append("dispatch:")
        o.append("  switch (tgt) {")
        for lab in sorted(labels):
            o.append(f"  case 0x{lab:08x}u: goto L_{lab:08x};")
        o.append("  default: if (ind) { rt_jump_ind(c, tgt, ret, ind); EXIT; } c->pend = 1; c->pend_target = tgt; EXIT;")
        o.append("  }")
        prev_end = None
        for idx, i in enumerate(self.ins):
            if prev_end is not None and prev_end != i.address and i.address not in labels:
                pass
            if i.address in labels:
                o.append(f"L_{i.address:08x}:")
            try:
                self.lift(i, live_after.get(i.address, ALL_FLAGS))
            except NotImplementedError as ex:
                self.e(f"rt_unimpl(c, 0x{i.address:08x}u, \"{i.mnemonic} {i.op_str}\"); /* {ex} */")
            prev_end = i.address + i.size
            nxt = idx + 1 < len(self.ins) and self.ins[idx + 1].address
            if nxt != prev_end and i.mnemonic not in ("jmp", "ret") :
                self.e(f"{{ rt_jump(c, 0x{prev_end:08x}u, ret); EXIT; }}")
        o.append("#undef EXIT")
        o.append("}")
        return "\n".join(o)

    # ---- instruction lifting ----
    def lift(self, i, live):
        m = i.mnemonic
        ops = i.operands
        nxt = i.address + i.size
        self.e(f"/* {i.address:08x}: {m} {i.op_str} */")

        if m in ("nop", "wait", "fwait", "emms", "cld_", "lfence"):
            return
        if m == "cld":
            self.e("c->df = 0;"); return
        if m == "std":
            self.e("c->df = 1;"); return
        if m == "mov":
            self.e(self.wr(ops[0], self.rd(ops[1], ops[0].size))); return
        if m in ("movzx", "movsx"):
            src = self.rd(ops[1])
            if m == "movsx":
                src = f"({UTYPE[ops[0].size]})({STYPE[ops[0].size]})({STYPE[ops[1].size]}){src}"
            self.e(self.wr(ops[0], f"({UTYPE[ops[0].size]}){src}")); return
        if m == "lea":
            self.e(self.wr(ops[0], f"({UTYPE[ops[0].size]}){self.addr(ops[1])}")); return
        if m == "xchg":
            ut = UTYPE[ops[0].size]
            self.e(f"{{ {ut} x0 = {self.rd(ops[0])}, x1 = {self.rd(ops[1])}; {self.wr(ops[0], 'x1')} {self.wr(ops[1], 'x0')} }}"); return
        if m == "bswap":
            self.e(self.wr(ops[0], f"_byteswap_ulong({self.rd(ops[0])})")); return
        if m == "push":
            if ops[0].size == 2:
                self.e(f"{{ uint16_t v = {self.rd(ops[0])}; c->esp -= 2; M16(c->esp) = v; }}"); return
            self.e(f"{{ uint32_t v = {self.rd(ops[0], 4)}; PUSH(v); }}"); return
        if m == "pop":
            if ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_ESP:
                self.e("c->esp = M32(c->esp);"); return
            self.e("t = M32(c->esp); c->esp += 4;")
            self.e(self.wr(ops[0], "t")); return
        if m == "pushfd":
            self.e("PUSH(rt_get_eflags(c));"); return
        if m == "popfd":
            self.e("rt_set_eflags(c, M32(c->esp)); c->esp += 4;"); return
        if m == "leave":
            self.e("c->esp = c->ebp; c->ebp = M32(c->esp); c->esp += 4;"); return
        if m == "cdq":
            self.e("c->edx = ((int32_t)c->eax < 0) ? 0xFFFFFFFFu : 0;"); return
        if m == "cwde":
            self.e("c->eax = (uint32_t)(int32_t)(int16_t)R16(0);"); return
        if m == "cbw":
            self.e("R16(0) = (uint16_t)(int16_t)(int8_t)R8L(0);"); return
        if m == "cwd":
            self.e("R16(2) = ((int16_t)R16(0) < 0) ? 0xFFFF : 0;"); return
        if m == "sahf":
            self.e("SF = (R8H(0) >> 7) & 1; ZF = (R8H(0) >> 6) & 1; PF = (R8H(0) >> 2) & 1; CF = R8H(0) & 1;"); return
        if m == "lahf":
            self.e("R8H(0) = (uint8_t)((SF << 7) | (ZF << 6) | (PF << 2) | 2 | CF);"); return
        if m == "cmc":
            self.e("CF = !CF;"); return
        if m == "clc":
            self.e("CF = 0;"); return
        if m == "stc":
            self.e("CF = 1;"); return
        if m == "cpuid":
            self.e("rt_cpuid(c);"); return

        if m in ("add", "sub", "cmp", "adc", "sbb"):
            return self.arith(i, m, ops, live)
        if m in ("and", "or", "xor", "test"):
            return self.logic(i, m, ops, live)
        if m in ("inc", "dec"):
            sz = ops[0].size; ut = UTYPE[sz]
            op = "+" if m == "inc" else "-"
            self.e(f"a = {self.rd(ops[0])}; r = ({ut})(a {op} 1);")
            if "of" in live:
                self.e(f"OF = ({ut})r == {MSB[sz]};" if m == "inc" else f"OF = ({ut})a == {MSB[sz]};")
            self.flags_res(live, "r", sz)
            self.e(self.wr(ops[0], f"({ut})r")); return
        if m == "neg":
            sz = ops[0].size; ut = UTYPE[sz]
            self.e(f"a = {self.rd(ops[0])}; r = ({ut})(0u - a);")
            if "cf" in live:
                self.e(f"CF = ({ut})a != 0;")
            if "of" in live:
                self.e(f"OF = ({ut})a == {MSB[sz]};")
            self.flags_res(live, "r", sz)
            self.e(self.wr(ops[0], f"({ut})r")); return
        if m == "not":
            ut = UTYPE[ops[0].size]
            self.e(self.wr(ops[0], f"({ut})~{self.rd(ops[0])}")); return
        if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            return self.shift(i, m, ops, live)
        if m in ("shld", "shrd"):
            sz = ops[0].size
            cnt = self.rd(ops[2], 1) if len(ops) == 3 else "R8L(1)"
            fnm = "rt_shld" if m == "shld" else "rt_shrd"
            self.e(f"{{ uint32_t n = ({cnt}) & 31; if (n) {{ {self.wr(ops[0], f'{fnm}(c, {self.rd(ops[0])}, {self.rd(ops[1])}, n)')} }} }}")
            return
        if m == "imul":
            return self.imul(i, ops, live)
        if m == "mul":
            sz = ops[0].size
            if sz == 4:
                self.e(f"q = (uint64_t)c->eax * {self.rd(ops[0])}; c->eax = (uint32_t)q; c->edx = (uint32_t)(q >> 32); CF = OF = c->edx != 0;")
            elif sz == 2:
                self.e(f"a = (uint32_t)R16(0) * {self.rd(ops[0])}; R16(0) = (uint16_t)a; R16(2) = (uint16_t)(a >> 16); CF = OF = (a >> 16) != 0;")
            else:
                self.e(f"R16(0) = (uint16_t)(R8L(0) * {self.rd(ops[0])}); CF = OF = R8H(0) != 0;")
            return
        if m in ("div", "idiv"):
            # A zero divisor or a quotient that does not fit the destination both raise #DE.
            sz = ops[0].size
            fault = f"rt_divzero(c, 0x{i.address:08x}u)"
            d = self.rd(ops[0])
            if m == "div":
                num = {1: "(uint64_t)R16(0)", 2: "(((uint64_t)R16(2) << 16) | R16(0))", 4: "(((uint64_t)c->edx << 32) | c->eax)"}[sz]
                lim = {1: "0xFFull", 2: "0xFFFFull", 4: "0xFFFFFFFFull"}[sz]
                self.e(f"{{ uint64_t n = {num}, dv = ({UTYPE[sz]}){d}, qv; if (!dv) {fault}; qv = n / dv; if (qv > {lim}) {fault};")
                rem = "n % dv"
            else:
                num = {1: "(int64_t)(int16_t)R16(0)", 2: "(int64_t)(int32_t)(((uint32_t)R16(2) << 16) | R16(0))",
                       4: "(int64_t)(((uint64_t)c->edx << 32) | c->eax)"}[sz]
                lo, hi = {1: ("-128", "127"), 2: ("-32768", "32767"), 4: ("-2147483648LL", "2147483647LL")}[sz]
                self.e(f"{{ int64_t n = {num}, dv = ({STYPE[sz]}){d}, qv; if (!dv || (n == INT64_MIN && dv == -1)) {fault}; qv = n / dv; if (qv < {lo} || qv > {hi}) {fault};")
                rem = "n % dv"
            if sz == 1:
                self.e(f"  R8L(0) = (uint8_t)qv; R8H(0) = (uint8_t)({rem}); }}")
            elif sz == 2:
                self.e(f"  R16(0) = (uint16_t)qv; R16(2) = (uint16_t)({rem}); }}")
            else:
                self.e(f"  c->eax = (uint32_t)qv; c->edx = (uint32_t)({rem}); }}")
            return
        if m in ("bt", "bts", "btr", "btc"):
            sz = ops[0].size
            if ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
                self.e(f"{{ int32_t bi = (int32_t){self.rd(ops[1])}; uint32_t ea = {self.addr(ops[0])} + (uint32_t)((bi >> 3) & ~3); uint32_t bit = (uint32_t)bi & 31; CF = (M32(ea) >> bit) & 1;"
                       + {"bt": "", "bts": " M32(ea) |= 1u << bit;", "btr": " M32(ea) &= ~(1u << bit);", "btc": " M32(ea) ^= 1u << bit;"}[m] + " }")
                return
            self.e(f"{{ uint32_t bit = {self.rd(ops[1])} & {sz * 8 - 1}; a = {self.rd(ops[0])}; CF = (a >> bit) & 1;")
            if m != "bt":
                expr = {"bts": "a | (1u << bit)", "btr": "a & ~(1u << bit)", "btc": "a ^ (1u << bit)"}[m]
                self.e(f"  {self.wr(ops[0], f'({UTYPE[sz]})({expr})')}")
            self.e("}"); return
        if m in ("bsf", "bsr"):
            fnm = "_BitScanForward" if m == "bsf" else "_BitScanReverse"
            self.e(f"{{ unsigned long idx; a = {self.rd(ops[1])}; ZF = a == 0; if (a) {{ {fnm}(&idx, a); {self.wr(ops[0], '(uint32_t)idx')} }} }}"); return
        if m in SETCC:
            self.e(self.wr(ops[0], f"(uint8_t)({SETCC[m]} ? 1 : 0)")); return

        if m == "lock cmpxchg" or m == "cmpxchg":
            sz = ops[0].size
            if ops[0].type != X.X86_OP_MEM or sz not in (1, 4):
                raise NotImplementedError("cmpxchg form")
            # Flags are those of 'cmp accumulator, old destination'.
            if sz == 1:
                self.e(f"{{ uint8_t acc = R8L(0), old = (uint8_t)_InterlockedCompareExchange8((volatile char*)(uintptr_t){self.addr(ops[0])}, (char){self.rd(ops[1])}, (char)acc); r = (uint8_t)(acc - old);"
                       f" CF = acc < old; OF = (((acc ^ old) & (acc ^ r)) & 0x80u) != 0; ZF = r == 0; SF = (r & 0x80u) != 0; PF = PARITY((uint8_t)r); if (!ZF) R8L(0) = old; }}")
                return
            self.e(f"{{ uint32_t acc = c->eax, old = (uint32_t)_InterlockedCompareExchange((volatile long*)(uintptr_t){self.addr(ops[0])}, (long){self.rd(ops[1])}, (long)acc); r = acc - old;"
                   f" CF = acc < old; OF = (((acc ^ old) & (acc ^ r)) >> 31) & 1; ZF = r == 0; SF = r >> 31; PF = PARITY((uint8_t)r); if (!ZF) c->eax = old; }}")
            return
        if m == "lock xadd" or m == "xadd":
            self.e(f"{{ uint32_t s = {self.rd(ops[1])}; uint32_t old = (uint32_t)_InterlockedExchangeAdd((volatile long*)(uintptr_t){self.addr(ops[0])}, (long)s); r = old + s; CF = r < old; OF = ((old ^ r) & (s ^ r)) >> 31; ZF = r == 0; SF = r >> 31; PF = PARITY((uint8_t)r); {self.wr(ops[1], 'old')} }}")
            return
        if m in ("lock inc", "lock dec"):
            fnm = "_InterlockedIncrement" if m == "lock inc" else "_InterlockedDecrement"
            self.e(f"{{ int32_t v = (int32_t){fnm}((volatile long*)(uintptr_t){self.addr(ops[0])}); ZF = v == 0; SF = v < 0; PF = PARITY((uint8_t)v); OF = v == {'(int32_t)0x80000000' if m == 'lock inc' else '0x7fffffff'}; }}")
            return

        if m in ("movsb", "movsw", "movsd", "rep movsb", "rep movsw", "rep movsd", "stosb", "stosw", "stosd",
                 "rep stosb", "rep stosw", "rep stosd", "lodsb", "lodsw", "lodsd", "repne scasb", "repne scasw",
                 "repe scasb", "repe cmpsb", "repe cmpsw", "repe cmpsd", "scasb", "cmpsb"):
            if m == "movsd" and ops and ops[0].type == X.X86_OP_REG:
                raise NotImplementedError("sse movsd")
            self.e(f"rt_str_{m.replace(' ', '_')}(c);")
            return
        if m == "movq":
            if ops[0].type == X.X86_OP_REG:
                self.e(f"{self.reg(ops[0].reg)} = {'M64(' + self.addr(ops[1]) + ')' if ops[1].type == X.X86_OP_MEM else self.reg(ops[1].reg)};")
            else:
                self.e(f"M64({self.addr(ops[0])}) = {self.reg(ops[1].reg)};")
            return

        if m.startswith("f"):
            return self.fpu(i, m, ops)

        # ---- control flow ----
        if m in JCC:
            t = ops[0].imm & 0xFFFFFFFF
            self.e(f"if ({JCC[m]}) {self.jump_to(t)}"); return
        if m == "jecxz":
            t = ops[0].imm & 0xFFFFFFFF
            self.e(f"if (c->ecx == 0) {self.jump_to(t)}"); return
        if m == "loop":
            t = ops[0].imm & 0xFFFFFFFF
            self.e(f"if (--c->ecx != 0) {self.jump_to(t)}"); return
        if m == "jmp":
            if ops[0].type == X.X86_OP_IMM:
                self.e(self.jump_to(ops[0].imm & 0xFFFFFFFF)); return
            self.e(f"tgt = {self.rd(ops[0], 4)}; ind = 1; goto dispatch;")
            return
        if m == "call":
            if ops[0].type == X.X86_OP_IMM:
                t = ops[0].imm & 0xFFFFFFFF
                self.e(f"PUSH(0x{nxt:08x}u); rt_call(c, 0x{t:08x}u, 0x{nxt:08x}u);")
            else:
                self.e(f"t = {self.rd(ops[0], 4)}; PUSH(0x{nxt:08x}u); rt_call(c, t, 0x{nxt:08x}u);")
            self.e(f"if (c->pend) {{ c->pend = 0; if (c->pend_target != 0x{nxt:08x}u) {{ tgt = c->pend_target; ind = 0; goto dispatch; }} }}")
            return
        if m == "ret":
            n = ops[0].imm if ops else 0
            self.e(f"t = M32(c->esp); c->esp += {4 + n}; if (t == ret) EXIT; tgt = t; ind = 2; goto dispatch;")
            return
        if m == "int3":
            self.e(f"rt_int3(c, 0x{i.address:08x}u);"); return
        raise NotImplementedError(m)

    def arith(self, i, m, ops, live):
        sz = ops[0].size; ut = UTYPE[sz]; msb = MSB[sz]
        b = self.rd(ops[1], sz)
        self.e(f"a = {self.rd(ops[0])}; b = {b};")
        if m == "add":
            self.e(f"r = ({ut})(a + b);")
            if "cf" in live: self.e(f"CF = r < ({ut})a;")
            if "of" in live: self.e(f"OF = (((a ^ r) & (b ^ r)) & {msb}) != 0;")
        elif m in ("sub", "cmp"):
            self.e(f"r = ({ut})(a - b);")
            if "cf" in live: self.e(f"CF = ({ut})a < ({ut})b;")
            if "of" in live: self.e(f"OF = (((a ^ b) & (a ^ r)) & {msb}) != 0;")
        elif m == "adc":
            self.e(f"{{ uint64_t w = (uint64_t)({ut})a + ({ut})b + CF; r = ({ut})w;")
            if "of" in live: self.e(f"  OF = (((a ^ r) & (b ^ r)) & {msb}) != 0;")
            self.e(f"  CF = (w >> {sz * 8}) != 0; }}")
        elif m == "sbb":
            self.e(f"{{ uint64_t w = (uint64_t)({ut})a - ({ut})b - CF; r = ({ut})w;")
            if "of" in live: self.e(f"  OF = (((a ^ b) & (a ^ r)) & {msb}) != 0;")
            self.e(f"  CF = (w >> 63) != 0; }}")
        self.flags_res(live, "r", sz)
        if m != "cmp":
            self.e(self.wr(ops[0], f"({ut})r"))

    def logic(self, i, m, ops, live):
        sz = ops[0].size; ut = UTYPE[sz]
        op = {"and": "&", "test": "&", "or": "|", "xor": "^"}[m]
        if m == "xor" and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
            self.e(self.wr(ops[0], "0"))
            if "zf" in live: self.e("ZF = 1;")
            if "sf" in live: self.e("SF = 0;")
            if "pf" in live: self.e("PF = 1;")
            if "cf" in live: self.e("CF = 0;")
            if "of" in live: self.e("OF = 0;")
            return
        self.e(f"r = ({ut})({self.rd(ops[0])} {op} {self.rd(ops[1], sz)});")
        if "cf" in live: self.e("CF = 0;")
        if "of" in live: self.e("OF = 0;")
        self.flags_res(live, "r", sz)
        if m != "test":
            self.e(self.wr(ops[0], f"({ut})r"))

    def shift(self, i, m, ops, live):
        sz = ops[0].size; ut = UTYPE[sz]; bits = sz * 8
        cnt = self.rd(ops[1], 1) if len(ops) > 1 else "1"
        self.e(f"{{ uint32_t n = ({cnt}) & 31; if (n) {{ a = {self.rd(ops[0])};")
        if m in ("shl", "sal"):
            self.e(f"  r = ({ut})(a << n); CF = n <= {bits} ? ((a >> ({bits} - n)) & 1) : 0; OF = ((r & {MSB[sz]}) != 0) ^ CF;")
        elif m == "shr":
            self.e(f"  r = ({ut})(({ut})a >> n); CF = (a >> (n - 1)) & 1; OF = (a & {MSB[sz]}) != 0;")
        elif m == "sar":
            self.e(f"  r = ({ut})(({STYPE[sz]})({ut})a >> (n > {bits - 1} ? {bits - 1} : n)); CF = (uint32_t)(({STYPE[sz]})({ut})a >> (n > {bits} ? {bits - 1} : n - 1)) & 1; OF = 0;")
        elif m == "rol":
            self.e(f"  n %= {bits}; r = ({ut})((({ut})a << n) | (({ut})a >> (({bits} - n) % {bits}))); CF = r & 1; OF = ((r & {MSB[sz]}) != 0) ^ CF;")
        elif m == "ror":
            self.e(f"  n %= {bits}; r = ({ut})((({ut})a >> n) | (({ut})a << (({bits} - n) % {bits}))); CF = (r & {MSB[sz]}) != 0; OF = CF ^ ((r >> {bits - 2}) & 1);")
        elif m == "rcl":
            self.e(f"  r = ({ut})a; while (n--) {{ uint32_t top = (r >> {bits - 1}) & 1; r = ({ut})((r << 1) | CF); CF = top; }} OF = ((r >> {bits - 1}) & 1) ^ CF;")
        elif m == "rcr":
            self.e(f"  r = ({ut})a; while (n--) {{ uint32_t low = r & 1; r = ({ut})((r >> 1) | ((uint32_t)CF << {bits - 1})); CF = low; }} OF = ((r >> {bits - 1}) ^ (r >> {bits - 2})) & 1;")
        if m in ("shl", "sal", "shr", "sar"):
            if "zf" in live: self.e(f"  ZF = ({ut})r == 0;")
            if "sf" in live: self.e(f"  SF = (r & {MSB[sz]}) != 0;")
            if "pf" in live: self.e(f"  PF = PARITY((uint8_t)r);")
        self.e("  " + self.wr(ops[0], f"({ut})r"))
        self.e("} }")

    def imul(self, i, ops, live):
        if len(ops) == 1:
            sz = ops[0].size
            if sz == 4:
                self.e(f"{{ int64_t p = (int64_t)(int32_t)c->eax * (int32_t){self.rd(ops[0])}; c->eax = (uint32_t)p; c->edx = (uint32_t)((uint64_t)p >> 32); CF = OF = p != (int32_t)p; }}")
            elif sz == 2:
                self.e(f"{{ int32_t p = (int16_t)R16(0) * (int16_t){self.rd(ops[0])}; R16(0) = (uint16_t)p; R16(2) = (uint16_t)((uint32_t)p >> 16); CF = OF = p != (int16_t)p; }}")
            else:
                self.e(f"{{ int16_t p = (int16_t)((int8_t)R8L(0) * (int8_t){self.rd(ops[0])}); R16(0) = (uint16_t)p; CF = OF = p != (int8_t)p; }}")
            return
        sz = ops[0].size
        if len(ops) == 2:
            x, y = self.rd(ops[0]), self.rd(ops[1], sz)
        else:
            x, y = self.rd(ops[1], sz), self.rd(ops[2], sz)
        st = STYPE[sz]
        self.e(f"{{ int64_t p = (int64_t)({st}){x} * ({st}){y}; CF = OF = p != ({st})p; {self.wr(ops[0], f'({UTYPE[sz]})p')} }}")

    def fpu(self, i, m, ops):
        def st_index(op):
            n = self.L.md.reg_name(op.reg)
            return int(n[3]) if n.startswith("st(") else 0

        def memval(op):
            a = self.addr(op)
            if m.startswith("fi"):
                return {2: f"(double)(int16_t)M16({a})", 4: f"(double)(int32_t)M32({a})", 8: f"(double)(int64_t)M64({a})"}[op.size]
            return {4: f"(double)MF32({a})", 8: f"MF64({a})", 10: f"rt_f80_load({a})"}[op.size]

        if m in ("fld", "fild"):
            if ops[0].type == X.X86_OP_REG:
                self.e(f"rt_fld_st(c, {st_index(ops[0])});")
            elif m == "fild" and ops[0].size == 8:
                self.e(f"FPUSH_I64(M64({self.addr(ops[0])}));")
            else:
                self.e(f"FPUSH({memval(ops[0])});")
            return
        consts = {"fld1": "1.0", "fldz": "0.0", "fldpi": "3.14159265358979323846", "fldl2e": "1.4426950408889634",
                  "fldln2": "0.6931471805599453", "fldlg2": "0.3010299956639812", "fldl2t": "3.3219280948873622"}
        if m in consts:
            self.e(f"FPUSH({consts[m]});"); return
        if m in ("fst", "fstp", "fist", "fistp", "fbstp"):
            pop = m.endswith("p")
            op = ops[0]
            if op.type == X.X86_OP_REG:
                self.e(f"rt_fcopy(c, {st_index(op)}, 0);")
            else:
                a = self.addr(op)
                if m.startswith("fist"):
                    self.e({2: f"M16({a}) = (uint16_t)rt_fist16(c, ST(0));", 4: f"M32({a}) = (uint32_t)rt_fist32(c, ST(0));",
                            8: f"M64({a}) = (uint64_t)FST_I64(c);"}[op.size])
                elif m == "fbstp":
                    self.e(f"rt_fbstp(c, {a}, ST(0));")
                else:
                    self.e({4: f"MF32({a}) = (float)ST(0);", 8: f"MF64({a}) = ST(0);", 10: f"rt_f80_store({a}, ST(0));"}[op.size])
            if pop:
                self.e("FPOP();")
            return
        arith = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/", "fsubr": "r-", "fdivr": "r/",
                 "fiadd": "+", "fisub": "-", "fimul": "*", "fidiv": "/", "fisubr": "r-", "fidivr": "r/"}
        base = m[:-1] if m.endswith("p") and m[:-1] in arith else m
        if base in arith:
            op = arith[base]
            rev = op.startswith("r")
            op = op[-1]
            pop = m.endswith("p") and base != m
            if len(ops) == 0:
                d, s = "ST(1)", "ST(0)"
                pop = True
            elif len(ops) == 1 and ops[0].type == X.X86_OP_MEM:
                d, s = "ST(0)", memval(ops[0])
            elif len(ops) == 1 and pop:
                d, s = f"ST({st_index(ops[0])})", "ST(0)"
            elif len(ops) == 1:
                d, s = "ST(0)", f"ST({st_index(ops[0])})"
            else:
                d, s = f"ST({st_index(ops[0])})", f"ST({st_index(ops[1])})"
            expr = f"({s}) {op} {d}" if rev else f"{d} {op} ({s})"
            self.e(f"{d} = {expr};")
            if pop:
                self.e("FPOP();")
            return
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp", "ficom", "ficomp", "ftst"):
            if m == "ftst":
                s = "0.0"
            elif not ops:
                s = "ST(1)"
            elif ops[0].type == X.X86_OP_MEM:
                s = memval(ops[0])
            else:
                s = f"ST({st_index(ops[0])})"
            self.e(f"rt_fcom(c, ST(0), {s});")
            if m.endswith("pp"):
                self.e("FPOP(); FPOP();")
            elif m.endswith("p"):
                self.e("FPOP();")
            return
        if m in ("fnstsw", "fstsw"):
            if ops[0].type == X.X86_OP_REG:
                self.e("R16(0) = rt_fsw(c);")
            else:
                self.e(f"M16({self.addr(ops[0])}) = rt_fsw(c);")
            return
        if m in ("fnstcw", "fstcw"):
            self.e(f"M16({self.addr(ops[0])}) = c->fcw;"); return
        if m == "fldcw":
            self.e(f"c->fcw = M16({self.addr(ops[0])});"); return
        if m in ("fnclex", "fclex"):
            self.e("c->fsw &= 0x7f00u;"); return
        if m in ("fninit", "finit"):
            self.e("rt_fninit(c);"); return
        if m == "fxch":
            k = max((st_index(o) for o in ops), default=1)
            self.e(f"rt_fxch(c, {k});"); return
        if m == "ffree":
            return
        unary = {"fchs": "-ST(0)", "fabs": "fabs(ST(0))", "fsqrt": "sqrt(ST(0))", "fsin": "sin(ST(0))",
                 "fcos": "cos(ST(0))", "frndint": "rt_frndint(c, ST(0))", "f2xm1": "(pow(2.0, ST(0)) - 1.0)"}
        if m in unary:
            self.e(f"ST(0) = {unary[m]};"); return
        if m == "fscale":
            self.e("ST(0) = ldexp(ST(0), (int)trunc(ST(1)));"); return
        if m == "fyl2x":
            self.e("ST(1) = ST(1) * log2(ST(0)); FPOP();"); return
        if m == "fpatan":
            self.e("ST(1) = atan2(ST(1), ST(0)); FPOP();"); return
        if m == "fptan":
            self.e("ST(0) = tan(ST(0)); FPUSH(1.0);"); return
        if m == "fsincos":
            self.e("{ double v = ST(0); ST(0) = sin(v); FPUSH(cos(v)); }"); return
        if m == "fprem":
            self.e("ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x400u;"); return
        raise NotImplementedError(m)


SANE = {"mov", "push", "pop", "add", "sub", "and", "or", "xor", "cmp", "test", "lea", "call", "jmp", "ret", "inc", "dec",
        "movzx", "movsx", "shl", "shr", "sar", "imul", "neg", "not", "xchg", "cdq", "nop", "leave", "fld", "fstp", "fild",
        "fwait", "wait", "sete", "setne"} | set(JCC)


def reloc_code_seeds(L, fns):
    """Relocated pointers into the code section that no function covers and that decode as plausible code
    (interface thunks, method pointers only referenced from data)."""
    spans = sorted((lo, hi) for _, _, rs in fns for lo, hi in rs)
    los = [lo for lo, _ in spans]

    def covered(x):
        k = bisect.bisect_right(los, x) - 1
        return k >= 0 and spans[k][0] <= x < spans[k][1]

    def entry_shape(t):
        # Delphi aligns every routine to 4 bytes, so an aligned relocated pointer is an entry whatever its first
        # instruction (empty virtual methods are a bare 'ret'); the sanity walk below still has to reach a ret/jmp.
        if t % 4 == 0:
            return True
        dec = L.disasm_range(t, t + 8)
        if len(dec) < 2:
            return False
        a, b = dec[0], dec[1]
        if a.mnemonic == "add" and b.mnemonic == "jmp" and a.op_str.split(",")[0] in ("eax", "dword ptr [esp + 4]"):
            return True
        if a.mnemonic == "push" and a.op_str == "ebp" and b.mnemonic == "mov" and b.op_str == "ebp, esp":
            return True
        if a.mnemonic == "push" and a.op_str in ("ebx", "esi", "edi") and b.mnemonic in ("push", "mov", "add", "sub", "xor"):
            return True
        # Leaf methods referenced only from a VMT: a few register operations and a ret ('and al, 0x1f; ret').
        if (a.mnemonic in SANE or a.mnemonic.startswith("f")) and any(r in a.op_str for r in ("eax", "edx", "ecx", "al", "ax")):
            pos = t
            for _ in range(24):
                dec = L.disasm_range(pos, pos + 1)
                if not dec or dec[0].mnemonic not in SANE and not dec[0].mnemonic.startswith("f"):
                    return False
                if dec[0].mnemonic == "ret":
                    return True
                if dec[0].mnemonic == "jmp":
                    return False
                pos += dec[0].size
        return False

    # A code address stored to memory by an instruction ('mov dword ptr [eax], 0x571b60' in a unit's init assigning a
    # procedural variable) is an entry point whatever its prologue looks like.
    stored = set()
    for slot, val in L.reloc_slots.items():
        if not (is_code(L, slot) and is_code(L, val)) or covered(val):
            continue
        for k in range(2, 11):
            dec = L.disasm_range(slot - k, slot - k + 1)
            if (dec and dec[0].address + dec[0].size == slot + 4 and dec[0].mnemonic == "mov"
                    and len(dec[0].operands) == 2 and dec[0].operands[0].type == X.X86_OP_MEM
                    and dec[0].operands[1].type == X.X86_OP_IMM and dec[0].operands[1].imm & 0xFFFFFFFF == val):
                stored.add(val)
                break

    out = sorted(stored)
    for t in sorted(L.reloc_targets):
        if t in stored or covered(t) or not entry_shape(t):
            continue
        pos, ok = t, False
        for _ in range(24):
            dec = L.disasm_range(pos, pos + 1)
            if not dec or dec[0].mnemonic not in SANE:
                break
            if dec[0].mnemonic in ("jmp", "ret"):
                ok = True
                break
            pos += dec[0].size
        if ok:
            out.append(t)
    print(f"{len(out)} relocation targets look like uncovered code")
    return out


def call_targets(L, fns):
    """Direct call targets that land mid-function become extra entry labels of the owning function;
    targets no function covers become new functions."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    owner_spans = sorted((lo, hi, e) for e, _, rs in fns for lo, hi in rs)
    los = [s[0] for s in owner_spans]
    entries = {e for e, _, _ in fns}
    extra, seeds = defaultdict(set), set()
    for e, _, rs in fns:
        for lo, hi in rs:
            for i in md.disasm(L.code[lo - L.code_lo: hi - L.code_lo], lo):
                if i.mnemonic != "call" or not i.op_str.startswith("0x"):
                    continue
                t = int(i.op_str, 16)
                if t in entries or not (L.code_lo <= t < L.code_hi):
                    continue
                k = bisect.bisect_right(los, t) - 1
                if k >= 0 and owner_spans[k][0] <= t < owner_spans[k][1]:
                    extra[owner_spans[k][2]].add(t)
                else:
                    seeds.add(t)
    print(f"{sum(len(v) for v in extra.values())} mid-function call targets, {len(seeds)} uncovered call targets")
    return extra, sorted(seeds)


def handler_entries(L, h):
    """A try handler is 'jmp HandleFinally/HandleAnyException; jmp @finally': @TryFinallyExit enters it at h + 5."""
    out = [h]
    if L.code[h - L.code_lo] == 0xE9:
        out.append(h + 5)
    return out


def descend(L, starts, may_enter):
    """Recursive-descent disassembly: follows direct branches, jump tables and try/except/finally handler pushes.
    Returns {address: end of instruction}. may_enter(pos) decides whether to keep walking into pos."""
    seen, work = {}, list(starts)
    while work:
        pos = work.pop()
        while pos not in seen and L.code_lo <= pos < L.code_hi and may_enter(pos):
            ins = L.disasm_range(pos, pos + 1)
            if not ins:
                break
            i = ins[0]
            seen[pos] = i.address + i.size
            m = i.mnemonic
            if (m.startswith("j") or m.startswith("loop")) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                work.append(i.operands[0].imm & 0xFFFFFFFF)
            work.extend(L.jump_table(i))
            # try/except and try/finally: 'push offset handler' followed by 'push dword ptr fs:[reg]'
            if m == "push" and i.operands and i.operands[0].type == X.X86_OP_IMM:
                t = i.operands[0].imm & 0xFFFFFFFF
                nxt = L.disasm_range(i.address + i.size, i.address + i.size + 1)
                if L.code_lo <= t < L.code_hi and nxt and nxt[0].mnemonic == "push" and "fs:" in nxt[0].op_str:
                    work.extend(handler_entries(L, t))
            if m == "jmp" or m == "ret" or m == "int3":
                break
            pos = i.address + i.size
    return seen


def to_ranges(seen):
    ranges = []
    for lo in sorted(seen):
        hi = seen[lo]
        if ranges and ranges[-1][1] >= lo:
            ranges[-1] = (ranges[-1][0], max(hi, ranges[-1][1]))
        else:
            ranges.append((lo, hi))
    return ranges


def extend_functions(L, fns):
    """Ghidra stops a function where it cannot follow control flow (an unresolved switch, a handler only reached by
    the exception dispatcher), so each function is re-walked from its own code and grows into whatever code no
    other function owns."""
    spans = sorted((lo, hi) for _, _, rs in fns for lo, hi in rs)
    los = [lo for lo, _ in spans]

    def owned(x):
        k = bisect.bisect_right(los, x) - 1
        return k >= 0 and spans[k][0] <= x < spans[k][1]

    entries = {e for e, _, _ in fns}
    out, grown = [], 0
    for e, name, rs in fns:
        mine = lambda x, rs=rs: any(lo <= x < hi for lo, hi in rs)
        seen = descend(L, [lo for lo, _ in rs], lambda x: mine(x) or (not owned(x) and x not in entries))
        extra = {a: b for a, b in seen.items() if not mine(a)}
        if extra:
            grown += 1
            rs = to_ranges({**{lo: hi for lo, hi in rs}, **extra})
        out.append((e, name, rs))
    print(f"{grown} functions extended past their Ghidra ranges")
    return out


def discover(L, fns, seeds):
    """Recursive-descent disassembly from seed addresses not already covered by a function."""
    covered = sorted((lo, hi) for _, _, rs in fns for lo, hi in rs)
    los = [lo for lo, _ in covered]
    entries = {e for e, _, _ in fns}

    def inside(x):
        k = bisect.bisect_right(los, x) - 1
        return k >= 0 and covered[k][0] <= x < covered[k][1]

    out = []
    for s in seeds:
        if s in entries or not (L.code_lo <= s < L.code_hi):
            continue
        ranges = to_ranges(descend(L, [s], lambda pos, s=s: pos == s or not inside(pos)))
        if ranges:
            out.append((s, f"seed_{s:08x}", ranges))
            entries.add(s)
    print(f"{len(out)} functions discovered from seeds")
    return out


def write_if_changed(path, text):
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return
    with open(path, "w") as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("functions")
    ap.add_argument("outdir")
    ap.add_argument("--seeds", help="extra function entry points (hex, one per line) found missing at runtime")
    a = ap.parse_args()

    L = Lifter(a.exe)
    fns = []
    for line in open(a.functions):
        entry, name, ranges = line.split()
        rs = []
        for r in ranges.split(","):
            lo, hi = (int(x, 16) for x in r.split("-"))
            lo, hi = max(lo, L.code_lo), min(hi, L.code_hi)
            if lo < hi:
                rs.append((lo, hi))
        e = int(entry, 16)
        if rs and L.code_lo <= e < L.code_hi:
            fns.append((e, name, rs))
    fns = extend_functions(L, fns)
    fns.extend(discover(L, fns, reloc_code_seeds(L, fns)))
    if a.seeds and os.path.exists(a.seeds):
        fns.extend(discover(L, fns, [int(x.split()[0], 16) for x in open(a.seeds) if x.strip() and not x.startswith("#")]))
    os.makedirs(a.outdir, exist_ok=True)

    def make_covered(fns):
        spans = sorted((lo, hi) for _, _, rs in fns for lo, hi in rs)
        span_los = [lo for lo, _ in spans]

        def covered(x):
            k = bisect.bisect_right(span_los, x) - 1
            return k >= 0 and spans[k][0] <= x < spans[k][1]
        return covered

    extra_labels = defaultdict(set)
    for _ in range(3):
        fns.sort()
        covered = make_covered(fns)
        owner, calls = {}, set()
        for e, name, rs in fns:
            g = FnGen(L, e, name, rs, covered)
            for i in g.ins:
                owner.setdefault(i.address, e)
                m = i.mnemonic
                if (m == "call" or m.startswith("j") or m.startswith("loop")) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                    t = i.operands[0].imm & 0xFFFFFFFF
                    if m == "call" or t not in g.addrs:
                        calls.add(t)
        entries = {e for e, _, _ in fns}
        seeds = []
        for t in calls:
            if t in entries or not (L.code_lo <= t < L.code_hi):
                continue
            if t in owner:
                extra_labels[owner[t]].add(t)
            else:
                seeds.append(t)
        # Any address the binary keeps as a pointer can be jumped to indirectly (finally continuations reached through
        # the RTL's handler, method pointers), so it must be a label in whichever function owns that instruction.
        for t in L.reloc_targets:
            if t in owner and t not in entries:
                extra_labels[owner[t]].add(t)
        for e, name, rs in fns:
            for lo, hi in rs:
                for i in L.disasm_range(lo, hi):
                    if i.mnemonic != "push" or not i.operands or i.operands[0].type != X.X86_OP_IMM:
                        continue
                    h = i.operands[0].imm & 0xFFFFFFFF
                    nxt = L.disasm_range(i.address + i.size, i.address + i.size + 1)
                    if L.code_lo <= h < L.code_hi and nxt and nxt[0].mnemonic == "push" and "fs:" in nxt[0].op_str:
                        for t in handler_entries(L, h):
                            if t in owner and t not in entries:
                                extra_labels[owner[t]].add(t)
        print(f"{sum(len(v) for v in extra_labels.values())} mid-function call targets, {len(seeds)} uncovered call targets")
        if not seeds:
            break
        fns.extend(discover(L, fns, sorted(seeds)))
    fns.sort()
    covered = make_covered(fns)

    table = []
    chunks, unimpl = defaultdict(list), defaultdict(int)

    absorbed = 0
    lifted = bytearray(L.code_hi - L.code_lo)
    for e, name, rs in fns:
        g = FnGen(L, e, name, rs, covered)
        g.extra_labels = extra_labels.get(e, set())
        absorbed += sum(1 for i in g.ins if not covered(i.address))
        for i in g.ins:
            lifted[i.address - L.code_lo: i.address - L.code_lo + i.size] = b"\1" * i.size
        src = g.gen()
        for line in g.out:
            if "rt_unimpl" in line:
                unimpl[line.split('"')[1].split()[0]] += 1
        chunks[e >> 15].append(src)
        for lab in sorted(g.labelset):
            table.append((lab, e))

    wanted = set()
    for key, srcs in chunks.items():
        name = f"fn_{key << 15:08x}.c"
        wanted.add(name)
        write_if_changed(os.path.join(a.outdir, name), '#include "rt.h"\n\n' + "\n\n".join(srcs) + "\n")
    for old in os.listdir(a.outdir):
        if old.startswith("fn_") and old.endswith(".c") and old not in wanted:
            os.remove(os.path.join(a.outdir, old))
    file_no = len(wanted)

    table = sorted(set(table), key=lambda le: (le[0], le[0] != le[1], le[1]))
    with io.StringIO() as f:
        f.write('#include "rt.h"\n\n')
        for e in sorted({e for _, e in table}):
            f.write(f"void f_{e:08x}(CPU*, uint32_t, uint32_t);\n")
        f.write(f"\nconst uint32_t rt_label_count = {len(table)};\n")
        f.write("const RtLabel rt_labels[] = {\n")
        for lab, e in table:
            f.write(f"  {{0x{lab:08x}u, f_{e:08x}, {1 if lab == e else 0}}},\n")
        f.write("};\n")
        f.write(f"const uint32_t rt_code_lo = 0x{L.code_lo:08x}u, rt_code_hi = 0x{L.code_hi:08x}u;\n")
        write_if_changed(os.path.join(a.outdir, "table.c"), f.getvalue())
    print(f"{len(fns)} functions, {len(table)} labels, {file_no} files")
    print("unimplemented:", dict(sorted(unimpl.items(), key=lambda kv: -kv[1])))
    report_coverage(L, lifted, os.path.join(a.outdir, "gaps.txt"))


def report_coverage(L, lifted, path):
    """Code-section bytes covered by lifted instructions; the uncovered runs that are not alignment padding go to
    gaps.txt (data embedded in code, such as VMTs and string literals, is expected there)."""
    total, gaps, pos = len(lifted), [], 0
    while pos < total:
        if lifted[pos]:
            pos += 1
            continue
        start = pos
        while pos < total and not lifted[pos]:
            pos += 1
        body = L.code[start:pos]
        if len(body) < 4 or all(b in (0x00, 0x90, 0xCC, 0x8B, 0xC0) for b in body):
            continue
        gaps.append((L.code_lo + start, pos - start))
    lifted_bytes = sum(lifted)
    gap_bytes = sum(n for _, n in gaps)
    with open(path, "w") as f:
        for a, n in gaps:
            f.write(f"{a:08x} {n}\n")
    print(f"coverage: {lifted_bytes}/{total} code bytes lifted ({100 * lifted_bytes / total:.1f}%), "
          f"{len(gaps)} non-padding gaps totalling {gap_bytes} bytes (see {path})")


if __name__ == "__main__":
    main()
