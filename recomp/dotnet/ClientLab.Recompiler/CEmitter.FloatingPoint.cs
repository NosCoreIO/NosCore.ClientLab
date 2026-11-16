using Iced.Intel;

namespace NosCore.ClientLab.Recompiler;

public sealed partial class CEmitter
{
    private void FloatingPoint(string m, X86Operand[] ops)
    {
        static int St(X86Operand operand) => operand.Register is >= Register.ST0 and <= Register.ST7 ? operand.Register - Register.ST0 : 0;
        string Value(X86Operand operand)
        {
            var address = Addr(operand);
            if (m.StartsWith("fi")) return $"(double)({S(operand.Size)})M{operand.Size * 8}({address})";
            return operand.Size switch { 4 => $"(double)MF32({address})", 8 => $"MF64({address})", 10 => $"rt_f80_load({address})", _ => throw new NotSupportedException("floating point memory size") };
        }
        if (m is "fld" or "fild")
        {
            E(ops[0].Kind == OperandKind.Register ? $"rt_fld_st(c, {St(ops[0])});" : m == "fild" && ops[0].Size == 8 ? $"FPUSH_I64(M64({Addr(ops[0])}));" : $"FPUSH({Value(ops[0])});"); return;
        }
        var constant = m switch { "fld1" => "1.0", "fldz" => "0.0", "fldpi" => "3.14159265358979323846", "fldl2e" => "1.4426950408889634", "fldln2" => "0.6931471805599453", "fldlg2" => "0.3010299956639812", "fldl2t" => "3.3219280948873622", _ => null };
        if (constant is not null) { E($"FPUSH({constant});"); return; }
        if (m is "fst" or "fstp" or "fist" or "fistp" or "fbstp")
        {
            var operand = ops[0]; var address = operand.Kind == OperandKind.Memory ? Addr(operand) : "";
            if (operand.Kind == OperandKind.Register) E($"rt_fcopy(c, {St(operand)}, 0);");
            else if (m.StartsWith("fist")) E(operand.Size switch { 2 => $"M16({address}) = (uint16_t)rt_fist16(c, ST(0));", 4 => $"M32({address}) = (uint32_t)rt_fist32(c, ST(0));", 8 => $"M64({address}) = (uint64_t)FST_I64(c);", _ => throw new NotSupportedException(m) });
            else if (m == "fbstp") E($"rt_fbstp(c, {address}, ST(0));");
            else E(operand.Size switch { 4 => $"MF32({address}) = (float)ST(0);", 8 => $"MF64({address}) = ST(0);", 10 => $"rt_f80_store({address}, ST(0));", _ => throw new NotSupportedException(m) });
            if (m.EndsWith('p')) E("FPOP();");
            return;
        }
        Dictionary<string, string> arithmetic = new() { ["fadd"]="+", ["fsub"]="-", ["fmul"]="*", ["fdiv"]="/", ["fsubr"]="r-", ["fdivr"]="r/", ["fiadd"]="+", ["fisub"]="-", ["fimul"]="*", ["fidiv"]="/", ["fisubr"]="r-", ["fidivr"]="r/" };
        var basis = m.EndsWith('p') && arithmetic.ContainsKey(m[..^1]) ? m[..^1] : m;
        if (arithmetic.TryGetValue(basis, out var op))
        {
            var reverse = op.StartsWith('r'); op = op[^1..]; var pop = m.EndsWith('p') && basis != m;
            string destination, source;
            if (ops.Length == 0) { destination = "ST(1)"; source = "ST(0)"; pop = true; }
            else if (ops.Length == 1 && ops[0].Kind == OperandKind.Memory) { destination = "ST(0)"; source = Value(ops[0]); }
            else if (ops.Length == 1 && pop) { destination = $"ST({St(ops[0])})"; source = "ST(0)"; }
            else if (ops.Length == 1) { destination = "ST(0)"; source = $"ST({St(ops[0])})"; }
            else { destination = $"ST({St(ops[0])})"; source = $"ST({St(ops[1])})"; }
            E(reverse ? $"{destination} = ({source}) {op} {destination};" : $"{destination} = {destination} {op} ({source});");
            if (pop) E("FPOP();"); return;
        }
        if (m is "fcom" or "fcomp" or "fcompp" or "fucom" or "fucomp" or "fucompp" or "ficom" or "ficomp" or "ftst")
        {
            // Iced exposes the implicit ST(0) as the first operand for register
            // comparisons. Capstone (used by the original lifter) exposes only ST(i).
            // Comparing with ops[0] therefore compared ST(0) with itself forever.
            var source = m == "ftst" ? "0.0" : ops.Length == 0 ? "ST(1)" : ops[^1].Kind == OperandKind.Memory ? Value(ops[^1]) : $"ST({St(ops[^1])})";
            E($"rt_fcom(c, ST(0), {source});");
            if (m.EndsWith("pp")) E("FPOP(); FPOP();"); else if (m.EndsWith('p')) E("FPOP();"); return;
        }
        switch (m)
        {
            case "fnstsw": case "fstsw": E(ops[0].Kind == OperandKind.Register ? "R16(0) = rt_fsw(c);" : $"M16({Addr(ops[0])}) = rt_fsw(c);"); return;
            case "fnstcw": case "fstcw": E($"M16({Addr(ops[0])}) = c->fcw;"); return;
            case "fldcw": E($"c->fcw = M16({Addr(ops[0])});"); return;
            case "fnclex": case "fclex": E("c->fsw &= 0x7f00u;"); return;
            case "fninit": case "finit": E("rt_fninit(c);"); return;
            case "fxch": E($"rt_fxch(c, {(ops.Length == 0 ? 1 : ops.Max(St))});"); return;
            case "ffree": return;
            case "fscale": E("ST(0) = ldexp(ST(0), (int)trunc(ST(1)));"); return;
            case "fyl2x": E("ST(1) = ST(1) * log2(ST(0)); FPOP();"); return;
            case "fpatan": E("ST(1) = atan2(ST(1), ST(0)); FPOP();"); return;
            case "fptan": E("ST(0) = tan(ST(0)); FPUSH(1.0);"); return;
            case "fsincos": E("{ double v = ST(0); ST(0) = sin(v); FPUSH(cos(v)); }"); return;
            case "fprem": E("ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x400u;"); return;
        }
        var unary = m switch { "fchs" => "-ST(0)", "fabs" => "fabs(ST(0))", "fsqrt" => "sqrt(ST(0))", "fsin" => "sin(ST(0))", "fcos" => "cos(ST(0))", "frndint" => "rt_frndint(c, ST(0))", "f2xm1" => "(pow(2.0, ST(0)) - 1.0)", _ => null };
        if (unary is not null) { E($"ST(0) = {unary};"); return; }
        throw new NotSupportedException(m);
    }
}
