using Iced.Intel;

namespace NosCore.ClientLab.Recompiler;

public sealed partial class CEmitter
{
    private void Lift(X86Instruction instruction)
    {
        var m = instruction.Mnemonic; var ops = instruction.Operands; var next = instruction.Next;
        string R(int index, int? size = null) => Read(ops[index], size);
        string W(int index, string value) => Write(ops[index], value);
        E($"/* {instruction.Address:x8}: {instruction.Text.Replace("*/", "")} */");
        switch (m)
        {
            case "nop": case "wait": case "fwait": case "emms": case "lfence": return;
            case "cld": E("c->df = 0;"); return;
            case "std": E("c->df = 1;"); return;
            case "mov": E(W(0, R(1, ops[0].Size))); return;
            case "movzx": E(W(0, $"({U(ops[0].Size)}){R(1)}")); return;
            case "movsx": E(W(0, $"({U(ops[0].Size)})({U(ops[0].Size)})({S(ops[0].Size)})({S(ops[1].Size)}){R(1)}")); return;
            case "lea": E(W(0, $"({U(ops[0].Size)}){Addr(ops[1])}")); return;
            case "xchg": E($"{{ {U(ops[0].Size)} x0 = {R(0)}, x1 = {R(1)}; {W(0, "x1")} {W(1, "x0")} }}"); return;
            case "bswap": E(W(0, $"_byteswap_ulong({R(0)})")); return;
            case "push":
                E(ops[0].Size == 2 ? $"{{ uint16_t v = {R(0)}; c->esp -= 2; M16(c->esp) = v; }}" : $"{{ uint32_t v = {R(0, 4)}; PUSH(v); }}"); return;
            case "pop":
                if (ops[0].Kind == OperandKind.Register && ops[0].Register == Register.ESP) { E("c->esp = M32(c->esp);"); return; }
                E("t = M32(c->esp); c->esp += 4;"); E(W(0, "t")); return;
            case "pushfd": E("PUSH(rt_get_eflags(c));"); return;
            case "popfd": E("rt_set_eflags(c, M32(c->esp)); c->esp += 4;"); return;
            case "leave": E("c->esp = c->ebp; c->ebp = M32(c->esp); c->esp += 4;"); return;
            case "cdq": E("c->edx = ((int32_t)c->eax < 0) ? 0xFFFFFFFFu : 0;"); return;
            case "cwde": E("c->eax = (uint32_t)(int32_t)(int16_t)R16(0);"); return;
            case "cbw": E("R16(0) = (uint16_t)(int16_t)(int8_t)R8L(0);"); return;
            case "cwd": E("R16(2) = ((int16_t)R16(0) < 0) ? 0xFFFF : 0;"); return;
            case "sahf": E("SF = (R8H(0) >> 7) & 1; ZF = (R8H(0) >> 6) & 1; PF = (R8H(0) >> 2) & 1; CF = R8H(0) & 1;"); return;
            case "lahf": E("R8H(0) = (uint8_t)((SF << 7) | (ZF << 6) | (PF << 2) | 2 | CF);"); return;
            case "cmc": E("CF = !CF;"); return;
            case "clc": E("CF = 0;"); return;
            case "stc": E("CF = 1;"); return;
            case "cpuid": E("rt_cpuid(c);"); return;
            case "add": case "sub": case "cmp": case "adc": case "sbb": Arithmetic(m, ops); return;
            case "and": case "or": case "xor": case "test": Logic(m, ops); return;
            case "inc": case "dec":
            {
                var size = ops[0].Size; var u = U(size);
                E($"a = {R(0)}; r = ({u})(a {(m == "inc" ? "+" : "-")} 1);");
                E($"OF = ({u}){(m == "inc" ? "r" : "a")} == {Msb(size)};");
                ResultFlags("r", size); E(W(0, $"({u})r")); return;
            }
            case "neg":
            {
                var size = ops[0].Size; var u = U(size);
                E($"a = {R(0)}; r = ({u})(0u - a); CF = ({u})a != 0; OF = ({u})a == {Msb(size)};");
                ResultFlags("r", size); E(W(0, $"({u})r")); return;
            }
            case "not": E(W(0, $"({U(ops[0].Size)})~{R(0)}")); return;
            case "shl": case "sal": case "shr": case "sar": case "rol": case "ror": case "rcl": case "rcr": Shift(m, ops); return;
            case "shld": case "shrd":
                E($"{{ uint32_t n = ({(ops.Length == 3 ? R(2, 1) : "R8L(1)")}) & 31; if (n) {{ {W(0, $"rt_{m}(c, {R(0)}, {R(1)}, n)")} }} }}"); return;
            case "imul": SignedMultiply(ops); return;
            case "mul":
                if (ops[0].Size == 4) E($"q = (uint64_t)c->eax * {R(0)}; c->eax = (uint32_t)q; c->edx = (uint32_t)(q >> 32); CF = OF = c->edx != 0;");
                else if (ops[0].Size == 2) E($"a = (uint32_t)R16(0) * {R(0)}; R16(0) = (uint16_t)a; R16(2) = (uint16_t)(a >> 16); CF = OF = (a >> 16) != 0;");
                else E($"R16(0) = (uint16_t)(R8L(0) * {R(0)}); CF = OF = R8H(0) != 0;");
                return;
            case "div": case "idiv": Divide(instruction, m, ops[0]); return;
            case "bt": case "bts": case "btr": case "btc":
            {
                var size = ops[0].Size;
                if (ops[0].Kind == OperandKind.Memory && ops[1].Kind == OperandKind.Register)
                {
                    E($"{{ int32_t bi = (int32_t){R(1)}; uint32_t ea = {Addr(ops[0])} + (uint32_t)((bi >> 3) & ~3); uint32_t bit = (uint32_t)bi & 31; CF = (M32(ea) >> bit) & 1;");
                    if (m != "bt") E("M32(ea) " + (m switch { "bts" => "|= 1u << bit;", "btr" => "&= ~(1u << bit);", _ => "^= 1u << bit;" }));
                    E("}"); return;
                }
                E($"{{ uint32_t bit = {R(1)} & {size * 8 - 1}; a = {R(0)}; CF = (a >> bit) & 1;");
                if (m != "bt") E(W(0, $"({U(size)})({(m switch { "bts" => "a | (1u << bit)", "btr" => "a & ~(1u << bit)", _ => "a ^ (1u << bit)" })})"));
                E("}"); return;
            }
            case "bsf": case "bsr":
                E($"{{ unsigned long idx; a = {R(1)}; ZF = a == 0; if (a) {{ {(m == "bsf" ? "_BitScanForward" : "_BitScanReverse")}(&idx, a); {W(0, "(uint32_t)idx")} }} }}"); return;
            case "lock cmpxchg": case "cmpxchg":
            {
                var size = ops[0].Size;
                if (ops[0].Kind != OperandKind.Memory || size is not (1 or 4)) throw new NotSupportedException("cmpxchg form");
                if (size == 1) E($"{{ uint8_t acc = R8L(0), old = (uint8_t)_InterlockedCompareExchange8((volatile char*)(uintptr_t){Addr(ops[0])}, (char){R(1)}, (char)acc); r = (uint8_t)(acc - old); CF = acc < old; OF = (((acc ^ old) & (acc ^ r)) & 0x80u) != 0; ZF = r == 0; SF = (r & 0x80u) != 0; PF = PARITY((uint8_t)r); if (!ZF) R8L(0) = old; }}");
                else E($"{{ uint32_t acc = c->eax, old = (uint32_t)_InterlockedCompareExchange((volatile long*)(uintptr_t){Addr(ops[0])}, (long){R(1)}, (long)acc); r = acc - old; CF = acc < old; OF = (((acc ^ old) & (acc ^ r)) >> 31) & 1; ZF = r == 0; SF = r >> 31; PF = PARITY((uint8_t)r); if (!ZF) c->eax = old; }}");
                return;
            }
            case "lock xadd": case "xadd":
                E($"{{ uint32_t s = {R(1)}; uint32_t old = (uint32_t)_InterlockedExchangeAdd((volatile long*)(uintptr_t){Addr(ops[0])}, (long)s); r = old + s; CF = r < old; OF = ((old ^ r) & (s ^ r)) >> 31; ZF = r == 0; SF = r >> 31; PF = PARITY((uint8_t)r); {W(1, "old")} }}"); return;
            case "lock inc": case "lock dec":
                E($"{{ int32_t v = (int32_t){(m == "lock inc" ? "_InterlockedIncrement" : "_InterlockedDecrement")}((volatile long*)(uintptr_t){Addr(ops[0])}); ZF = v == 0; SF = v < 0; PF = PARITY((uint8_t)v); OF = v == {(m == "lock inc" ? "(int32_t)0x80000000" : "0x7fffffff")}; }}"); return;
            case "movq":
                E(ops[0].Kind == OperandKind.Register ? $"{Reg(ops[0].Register)} = {(ops[1].Kind == OperandKind.Memory ? $"M64({Addr(ops[1])})" : Reg(ops[1].Register))};" : $"M64({Addr(ops[0])}) = {Reg(ops[1].Register)};"); return;
            case "jecxz": E($"if (c->ecx == 0) {Jump((uint)ops[0].Immediate)}"); return;
            case "loop": E($"if (--c->ecx != 0) {Jump((uint)ops[0].Immediate)}"); return;
            case "jmp":
                E(ops[0].Kind == OperandKind.Immediate ? Jump((uint)ops[0].Immediate) : $"tgt = {R(0, 4)}; ind = 1; goto dispatch;"); return;
            case "call":
                if (ops[0].Kind == OperandKind.Immediate)
                {
                    var target = (uint)ops[0].Immediate;
                    if (CallOwners.TryGetValue(target, out var owner)) { Callees.Add(owner); E($"PUSH(0x{next:x8}u); f_{owner:x8}(c, 0x{target:x8}u, 0x{next:x8}u);"); }
                    else E($"PUSH(0x{next:x8}u); rt_call(c, 0x{target:x8}u, 0x{next:x8}u);");
                }
                else E($"t = {R(0, 4)}; PUSH(0x{next:x8}u); rt_call(c, t, 0x{next:x8}u);");
                E($"if (c->pend) {{ c->pend = 0; if (c->pend_target != 0x{next:x8}u) {{ tgt = c->pend_target; ind = 0; goto dispatch; }} }}"); return;
            case "ret": E($"t = M32(c->esp); c->esp += {4 + (ops.Length > 0 ? ops[0].Immediate : 0)}; if (t == ret) EXIT; tgt = t; ind = 2; goto dispatch;"); return;
            case "int3": E($"rt_int3(c, 0x{instruction.Address:x8}u);"); return;
        }
        if (Conditions.TryGetValue(m, out var condition)) { E($"if ({condition}) {Jump((uint)ops[0].Immediate)}"); return; }
        if (m.StartsWith("set") && Conditions.TryGetValue("j" + m[3..], out condition)) { E(W(0, $"(uint8_t)({condition} ? 1 : 0)")); return; }
        string[] strings = ["movsb", "movsw", "movsd", "rep movsb", "rep movsw", "rep movsd", "stosb", "stosw", "stosd", "rep stosb", "rep stosw", "rep stosd", "lodsb", "lodsw", "lodsd", "repne scasb", "repne scasw", "repe scasb", "repe cmpsb", "repe cmpsw", "repe cmpsd", "scasb", "cmpsb"];
        if (strings.Contains(m))
        {
            if (m == "movsd" && ops.Length > 0 && ops[0].Kind == OperandKind.Register) throw new NotSupportedException("SSE movsd");
            E($"rt_str_{m.Replace(' ', '_')}(c);"); return;
        }
        if (m.StartsWith('f')) { FloatingPoint(m, ops); return; }
        throw new NotSupportedException(m);
    }

    private void Arithmetic(string m, X86Operand[] ops)
    {
        var size = ops[0].Size; var u = U(size); var msb = Msb(size);
        E($"a = {Read(ops[0])}; b = {Read(ops[1], size)};");
        switch (m)
        {
            case "add": E($"r = ({u})(a + b); CF = r < ({u})a; OF = (((a ^ r) & (b ^ r)) & {msb}) != 0;"); break;
            case "sub": case "cmp": E($"r = ({u})(a - b); CF = ({u})a < ({u})b; OF = (((a ^ b) & (a ^ r)) & {msb}) != 0;"); break;
            case "adc": E($"{{ uint64_t w = (uint64_t)({u})a + ({u})b + CF; r = ({u})w; OF = (((a ^ r) & (b ^ r)) & {msb}) != 0; CF = (w >> {size * 8}) != 0; }}"); break;
            case "sbb": E($"{{ uint64_t w = (uint64_t)({u})a - ({u})b - CF; r = ({u})w; OF = (((a ^ b) & (a ^ r)) & {msb}) != 0; CF = (w >> 63) != 0; }}"); break;
        }
        ResultFlags("r", size);
        if (m != "cmp") E(Write(ops[0], $"({u})r"));
    }

    private void Logic(string m, X86Operand[] ops)
    {
        var size = ops[0].Size;
        if (m == "xor" && ops[0].Kind == OperandKind.Register && ops[1].Kind == OperandKind.Register && ops[0].Register == ops[1].Register)
        { E(Write(ops[0], "0")); E("ZF = 1; SF = 0; PF = 1; CF = 0; OF = 0;"); return; }
        var op = m switch { "and" or "test" => "&", "or" => "|", _ => "^" };
        E($"r = ({U(size)})({Read(ops[0])} {op} {Read(ops[1], size)}); CF = 0; OF = 0;"); ResultFlags("r", size);
        if (m != "test") E(Write(ops[0], $"({U(size)})r"));
    }

    private void Shift(string m, X86Operand[] ops)
    {
        var size = ops[0].Size; var u = U(size); var bits = size * 8;
        E($"{{ uint32_t n = ({(ops.Length > 1 ? Read(ops[1], 1) : "1")}) & 31; if (n) {{ a = {Read(ops[0])};");
        E(m switch
        {
            "shl" or "sal" => $"r = ({u})(a << n); CF = n <= {bits} ? ((a >> ({bits} - n)) & 1) : 0; OF = ((r & {Msb(size)}) != 0) ^ CF;",
            "shr" => $"r = ({u})(({u})a >> n); CF = (a >> (n - 1)) & 1; OF = (a & {Msb(size)}) != 0;",
            "sar" => $"r = ({u})(({S(size)})({u})a >> (n > {bits - 1} ? {bits - 1} : n)); CF = (uint32_t)(({S(size)})({u})a >> (n > {bits} ? {bits - 1} : n - 1)) & 1; OF = 0;",
            "rol" => $"n %= {bits}; r = ({u})((({u})a << n) | (({u})a >> (({bits} - n) % {bits}))); CF = r & 1; OF = ((r & {Msb(size)}) != 0) ^ CF;",
            "ror" => $"n %= {bits}; r = ({u})((({u})a >> n) | (({u})a << (({bits} - n) % {bits}))); CF = (r & {Msb(size)}) != 0; OF = CF ^ ((r >> {bits - 2}) & 1);",
            "rcl" => $"r = ({u})a; while (n--) {{ uint32_t top = (r >> {bits - 1}) & 1; r = ({u})((r << 1) | CF); CF = top; }} OF = ((r >> {bits - 1}) & 1) ^ CF;",
            "rcr" => $"r = ({u})a; while (n--) {{ uint32_t low = r & 1; r = ({u})((r >> 1) | ((uint32_t)CF << {bits - 1})); CF = low; }} OF = ((r >> {bits - 1}) ^ (r >> {bits - 2})) & 1;",
            _ => throw new NotSupportedException(m)
        });
        if (m is "shl" or "sal" or "shr" or "sar") ResultFlags("r", size);
        E(Write(ops[0], $"({u})r")); E("} }");
    }

    private void SignedMultiply(X86Operand[] ops)
    {
        var size = ops[0].Size;
        if (ops.Length == 1)
        {
            E(size switch
            {
                4 => $"{{ int64_t p = (int64_t)(int32_t)c->eax * (int32_t){Read(ops[0])}; c->eax = (uint32_t)p; c->edx = (uint32_t)((uint64_t)p >> 32); CF = OF = p != (int32_t)p; }}",
                2 => $"{{ int32_t p = (int16_t)R16(0) * (int16_t){Read(ops[0])}; R16(0) = (uint16_t)p; R16(2) = (uint16_t)((uint32_t)p >> 16); CF = OF = p != (int16_t)p; }}",
                _ => $"{{ int16_t p = (int16_t)((int8_t)R8L(0) * (int8_t){Read(ops[0])}); R16(0) = (uint16_t)p; CF = OF = p != (int8_t)p; }}"
            }); return;
        }
        var x = Read(ops[ops.Length == 2 ? 0 : 1], size); var y = Read(ops[ops.Length == 2 ? 1 : 2], size);
        E($"{{ int64_t p = (int64_t)({S(size)}){x} * ({S(size)}){y}; CF = OF = p != ({S(size)})p; {Write(ops[0], $"({U(size)})p")} }}");
    }

    private void Divide(X86Instruction instruction, string m, X86Operand operand)
    {
        var size = operand.Size; var fault = $"rt_divzero(c, 0x{instruction.Address:x8}u)";
        if (m == "div")
        {
            var number = size switch { 1 => "(uint64_t)R16(0)", 2 => "(((uint64_t)R16(2) << 16) | R16(0))", _ => "(((uint64_t)c->edx << 32) | c->eax)" };
            var limit = size switch { 1 => "0xFFull", 2 => "0xFFFFull", _ => "0xFFFFFFFFull" };
            E($"{{ uint64_t n = {number}, dv = ({U(size)}){Read(operand)}, qv; if (!dv) {fault}; qv = n / dv; if (qv > {limit}) {fault};");
        }
        else
        {
            var number = size switch { 1 => "(int64_t)(int16_t)R16(0)", 2 => "(int64_t)(int32_t)(((uint32_t)R16(2) << 16) | R16(0))", _ => "(int64_t)(((uint64_t)c->edx << 32) | c->eax)" };
            var bounds = size switch { 1 => ("-128", "127"), 2 => ("-32768", "32767"), _ => ("-2147483648LL", "2147483647LL") };
            E($"{{ int64_t n = {number}, dv = ({S(size)}){Read(operand)}, qv; if (!dv || (n == INT64_MIN && dv == -1)) {fault}; qv = n / dv; if (qv < {bounds.Item1} || qv > {bounds.Item2}) {fault};");
        }
        E(size switch { 1 => "R8L(0) = (uint8_t)qv; R8H(0) = (uint8_t)(n % dv); }", 2 => "R16(0) = (uint16_t)qv; R16(2) = (uint16_t)(n % dv); }", _ => "c->eax = (uint32_t)qv; c->edx = (uint32_t)(n % dv); }" });
    }
}
