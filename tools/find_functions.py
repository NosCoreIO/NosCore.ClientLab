#!/usr/bin/env python3
"""Write functions.txt without Ghidra, for machines where it cannot be installed.

usage: tools/find_functions.py <NosCore.exe> <functions.txt>

Recursive descent with the lifter's own decoder, in order of trust:

1. the PE entry point, the unit init table and everything they reach through direct calls;
2. relocated pointers into the code section that open with a routine prologue, and what those call;
3. the other relocated pointers the lifter thinks look like code, and what those call;
4. every remaining aligned relocated pointer (callbacks and methods without a prologue).

A candidate is dropped when its walk decodes an opcode no compiler emits, runs off the section, or cuts through an
instruction an accepted function already owns. That is what a pointer into a data table does, and accepting one
lets it claim bytes inside real functions. Code two functions share belongs to the one accepted first.

The output has the "entry name start-end[,start-end...]" lines tools/decompile.sh produces.
"""
import os
import sys
from collections import deque

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "recomp"))
import lift  # noqa: E402

X = lift.X
START, INSIDE = 1, 2


class Finder:
    def __init__(self, L):
        self.L = L
        self.owned = bytearray(L.code_hi - L.code_lo)
        self.functions = {}
        self.rejected = set()
        self.trusted = set()

    def walk(self, entry, strict):
        """{address: end} of the instructions only this entry reaches and its direct call targets, or None when a
        strict walk is invalid. A walk that is not strict, or one that meets garbage straight after a call (the
        callee never returns: Halt, a raise), just ends that path there."""
        L, owned = self.L, self.owned
        seen, calls, work = {}, set(), [(entry, False)]
        while work:
            pos, after_call = work.pop()
            while pos not in seen:
                bad = not (L.code_lo <= pos < L.code_hi) or owned[pos - L.code_lo] == INSIDE
                ins = None if bad else L.disasm_range(pos, pos + 1)
                if not bad and (owned[pos - L.code_lo] == START or (pos != entry and pos in self.functions)):
                    break
                if not bad:
                    # Opcode 0x00 is 'add byte ptr [mem], reg8'. Compilers all but never emit it, and it is what the
                    # zero bytes of padding and of small constants decode to (a 4-byte constant in front of a routine,
                    # with a pointer to it, would otherwise pass for the routine's first instructions).
                    bad = not ins or ins[0].mnemonic in lift.JUNK_OPS or ins[0].opcode[0] == 0 or any(
                        owned[a - L.code_lo] for a in range(pos + 1, min(ins[0].address + ins[0].size, L.code_hi)))
                if not bad and not self.relocations_fit(ins[0]):
                    bad = True
                if bad:
                    if strict and not after_call:
                        return None
                    break
                i = ins[0]
                end = i.address + i.size
                seen[pos] = end
                m = i.mnemonic
                if i.operands and i.operands[0].type == X.X86_OP_IMM:
                    t = i.operands[0].imm & 0xFFFFFFFF
                    if m == "call":
                        if L.code_lo <= t < L.code_hi:
                            calls.add(t)
                    elif m.startswith("j") or m.startswith("loop"):
                        work.append((t, False))
                    elif m == "push" and L.code_lo <= t < L.code_hi:
                        nxt = L.disasm_range(end, end + 1)
                        if nxt and nxt[0].mnemonic == "push" and "fs:" in nxt[0].op_str:
                            work.extend((h, False) for h in lift.handler_entries(L, t))
                work.extend((t, False) for t in L.jump_table(i))
                if m in ("jmp", "ret", "int3"):
                    break
                after_call = m == "call"
                pos = end
        # Two paths of one walk can still disagree about where instructions start.
        kept = None
        for a in sorted(seen):
            if kept is not None and seen[kept] > a:
                if strict:
                    return None
                del seen[a]
            else:
                kept = a
        return seen, calls

    def relocations_fit(self, i):
        """Every relocated 4-byte slot an instruction touches is exactly its address or immediate field. A decode that
        starts at the wrong byte, or runs through data, puts those slots anywhere else."""
        slots = self.L.reloc_slots
        end = i.address + i.size
        for a in range(i.address - 3, end):
            if a not in slots:
                continue
            fits = (i.disp_size == 4 and a == i.address + i.disp_offset) or (i.imm_size == 4 and a == i.address + i.imm_offset)
            if not fits:
                return False
        return True

    def accept(self, seeds, strict):
        """Accepts each seed that walks cleanly, then what it calls. Returns how many functions were added.
        What an accepted function calls directly is code, so those walks are never strict."""
        added, queue = 0, deque((s, strict) for s in seeds)
        while queue:
            e, check = queue.popleft()
            if e in self.functions or not (self.L.code_lo <= e < self.L.code_hi):
                continue
            if e in self.rejected and check:
                continue
            if self.owned[e - self.L.code_lo] == INSIDE and not check and self.evict(e):
                pass
            elif self.owned[e - self.L.code_lo] != 0:
                # Already inside an accepted function: the lifter makes it a label there.
                continue
            result = self.walk(e, check)
            if result is None or not result[0]:
                self.rejected.add(e)
                continue
            seen, calls = result
            for a, end in seen.items():
                self.owned[a - self.L.code_lo] = START
                for k in range(a + 1, end):
                    self.owned[k - self.L.code_lo] = INSIDE
            self.functions[e] = lift.to_ranges(seen)
            self.rejected.discard(e)
            if not check:
                self.trusted.add(e)
            added += 1
            queue.extend((t, False) for t in sorted(calls))
        return added

    def evict(self, address):
        """A direct call lands in the middle of an instruction of a function that only came from a pointer: that
        function was data. Removes it so the call target can be walked. False when the owner is trusted."""
        owner = next((e for e, rs in self.functions.items() if any(lo <= address < hi for lo, hi in rs)), None)
        if owner is None or owner in self.trusted:
            return False
        for lo, hi in self.functions.pop(owner):
            self.owned[lo - self.L.code_lo: hi - self.L.code_lo] = bytes(hi - lo)
        self.rejected.add(owner)
        return True

    def as_lifter_functions(self):
        return [(e, f"fn_{e:08x}", rs) for e, rs in sorted(self.functions.items())]


def split_huge(L, functions, limit=2500, piece=600):
    """Lists a routine with thousands of branch points as several functions. Each call or jump becomes an entry label
    in the lifted C, and the WebAssembly backend's irreducible control flow pass is quadratic in those: one routine
    with 5000 of them (the client's packet dispatcher) takes hours to compile. A jump from one piece to the next is an
    ordinary jump out of a function to the lifter."""
    out = []
    for e, name, ranges in functions:
        if sum(hi - lo for lo, hi in ranges) < 16384:
            out.append((e, name, ranges))
            continue
        ins = [i for lo, hi in ranges for i in L.disasm_range(lo, hi)]
        branchy = lambda i: i.mnemonic == "call" or i.mnemonic.startswith("j") or i.mnemonic in ("ret", "loop")
        if sum(1 for i in ins if branchy(i)) <= limit:
            out.append((e, name, ranges))
            continue
        pieces, cur, count = [], [], 0
        for i in ins:
            cur.append(i)
            count += branchy(i)
            # Cut after an unconditional jump when one comes soon enough, so most pieces end without falling through.
            if count >= piece and (i.mnemonic in ("jmp", "ret") or count >= piece + 200):
                pieces.append(cur)
                cur, count = [], 0
        if cur:
            pieces.append(cur)
        # The entry keeps the routine's first instruction; with several ranges that is not always the lowest address.
        for k, part in enumerate(pieces):
            start = e if any(i.address == e for i in part) else part[0].address
            seen = {i.address: i.address + i.size for i in part}
            out.append((start, f"fn_{start:08x}", lift.to_ranges(seen)))
        print(f"{e:08x} listed as {len(pieces)} functions")
    return sorted(out)


def has_prologue(L, t):
    head = L.disasm_range(t, t + 2)
    if len(head) < 2 or head[0].mnemonic != "push" or head[1].opcode[0] == 0:
        return False
    return head[0].op_str in ("ebp", "ebx", "esi", "edi") and head[1].mnemonic in ("mov", "push", "add", "sub", "xor")


def main():
    exe, out = sys.argv[1], sys.argv[2]
    L = lift.Lifter(exe)
    finder = Finder(L)
    entry = L.base + L.pe.OPTIONAL_HEADER.AddressOfEntryPoint
    finder.accept([entry, *lift.init_table_seeds(L)], strict=False)
    print(f"{len(finder.functions)} functions reachable through direct calls", flush=True)

    strong = sorted(t for t in L.reloc_targets if t % 4 == 0 and has_prologue(L, t))
    finder.accept(strong, strict=True)
    print(f"{len(finder.functions)} functions after relocated pointers to a prologue", flush=True)

    for _ in range(4):
        candidates = [t for t in lift.reloc_code_seeds(L, finder.as_lifter_functions())
                      if t not in finder.functions and t not in finder.rejected]
        if not finder.accept(candidates, strict=True):
            break
    print(f"{len(finder.functions)} functions after the lifter's own candidates", flush=True)

    # Callbacks and methods with no prologue ('mov ecx, [x]; ...; ret') are only ever reached through a pointer.
    # Delphi aligns routines to 4 bytes, and the strict walk throws out the pointers that lead into data.
    finder.accept(sorted(t for t in L.reloc_targets if t % 4 == 0), strict=True)
    print(f"{len(finder.functions)} functions in all, {len(finder.rejected)} candidates dropped", flush=True)

    listed = split_huge(L, finder.as_lifter_functions())
    with open(out, "w") as f:
        for e, name, ranges in listed:
            f.write(f"{e:x} {name} {','.join(f'{lo:x}-{hi:x}' for lo, hi in ranges)}\n")
    print(f"{len(listed)} functions in {out}")


if __name__ == "__main__":
    main()
