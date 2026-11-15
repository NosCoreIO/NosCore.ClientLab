using System.Text;
using Iced.Intel;

namespace NosCore.ClientLab.Recompiler;

public sealed partial class CEmitter
{
    private readonly ExecutableImage image;
    private readonly FunctionDefinition function;
    private readonly List<X86Instruction> instructions;
    private readonly HashSet<uint> addresses;
    private readonly StringBuilder output = new();
    public HashSet<uint> Labels { get; private set; } = [];
    public HashSet<uint> ExtraLabels { get; set; } = [];
    public IReadOnlyDictionary<uint, uint> CallOwners { get; set; } = new Dictionary<uint, uint>();
    public HashSet<uint> Callees { get; } = [];
    public List<string> Unsupported { get; } = [];
    public IReadOnlyList<X86Instruction> Instructions => instructions;
    public uint Entry => function.Entry;
    public bool UsesSeh => instructions.Any(i => i.WritesFsEsp);

    public static readonly Dictionary<string, string> Conditions = new()
    {
        ["je"]="ZF", ["jne"]="!ZF", ["jb"]="CF", ["jae"]="!CF", ["jbe"]="(CF||ZF)", ["ja"]="(!CF&&!ZF)",
        ["jl"]="(SF!=OF)", ["jge"]="(SF==OF)", ["jle"]="(ZF||SF!=OF)", ["jg"]="(!ZF&&SF==OF)",
        ["js"]="SF", ["jns"]="!SF", ["jo"]="OF", ["jno"]="!OF", ["jp"]="PF", ["jnp"]="!PF"
    };
    private static readonly string[] Registers32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"];
    private static readonly string[] Registers16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"];
    private static readonly string[] Registers8L = ["al", "cl", "dl", "bl"];
    private static readonly string[] Registers8H = ["ah", "ch", "dh", "bh"];
    private static string U(int size) => $"uint{size * 8}_t";
    private static string S(int size) => $"int{size * 8}_t";
    private static string Msb(int size) => size switch { 1 => "0x80u", 2 => "0x8000u", 4 => "0x80000000u", _ => throw new NotSupportedException("operand size") };
    private void E(string text) => output.Append("  ").AppendLine(text);

    public CEmitter(ExecutableImage image, FunctionDefinition function, Func<uint, bool>? covered = null)
    {
        this.image = image; this.function = function;
        instructions = function.Ranges.SelectMany(r => image.Disassemble(r.Start, r.End)).ToList();
        addresses = instructions.Select(i => i.Address).ToHashSet();
        if (covered is not null) AbsorbOrphans(covered);
        instructions.Sort((a, b) => a.Address.CompareTo(b.Address));
    }

    private bool IsCodePush(int index)
    {
        var previous = index > 0 ? instructions[index - 1] : null;
        var next = index + 1 < instructions.Count ? instructions[index + 1] : null;
        return previous is { Mnemonic: "mov" } && previous.Operands.FirstOrDefault() is { Kind: OperandKind.Memory, Segment: Register.FS } || next?.IsFsPush == true;
    }

    private void AbsorbOrphans(Func<uint, bool> covered)
    {
        bool changed;
        do
        {
            changed = false;
            instructions.Sort((a, b) => a.Address.CompareTo(b.Address));
            List<(uint Address, bool Force)> roots = [];
            for (var index = 0; index < instructions.Count; index++)
            {
                var instruction = instructions[index];
                roots.AddRange(image.JumpTable(instruction).Where(t => !addresses.Contains(t)).Select(t => (t, true)));
                if (instruction.DirectTarget is not { } target || instruction.Mnemonic == "push" && !IsCodePush(index)) continue;
                if ((instruction.Mnemonic == "push" || instruction.Mnemonic.StartsWith('j') || instruction.Mnemonic.StartsWith("loop")) && image.ContainsCode(target) && !addresses.Contains(target) && (instruction.Mnemonic == "push" || !covered(target)))
                    roots.Add((target, instruction.Mnemonic == "push"));
            }
            foreach (var root in roots.ToArray())
                if (root.Force && image.Decode(root.Address) is { Mnemonic: "jmp" } handler) roots.Add((handler.Next, true));
            foreach (var root in roots)
            {
                var work = new Stack<(uint Address, bool Force)>(); work.Push(root);
                while (work.TryPop(out var current))
                {
                    var address = current.Address;
                    while (image.ContainsCode(address) && !addresses.Contains(address) && (current.Force || !covered(address)))
                    {
                        var instruction = image.Decode(address); if (instruction is null) break;
                        instructions.Add(instruction); addresses.Add(address); changed = true;
                        if ((instruction.Mnemonic.StartsWith('j') || instruction.Mnemonic.StartsWith("loop")) && instruction.DirectTarget is { } target) work.Push((target, current.Force));
                        foreach (var jumpTarget in image.JumpTable(instruction)) work.Push((jumpTarget, true));
                        if (instruction.Mnemonic is "jmp" or "ret" or "int3") break;
                        address = instruction.Next;
                    }
                }
            }
        } while (changed);
    }

    private HashSet<uint> FindLabels()
    {
        HashSet<uint> labels = [];
        if (addresses.Contains(function.Entry)) labels.Add(function.Entry);
        labels.UnionWith(ExtraLabels.Where(addresses.Contains));
        for (var index = 0; index < instructions.Count; index++)
        {
            var instruction = instructions[index]; var m = instruction.Mnemonic;
            if ((m.StartsWith('j') || m is "loop" or "call" or "push") && instruction.DirectTarget is { } target && addresses.Contains(target)) labels.Add(target);
            if ((m.StartsWith('j') || m is "ret" or "call" or "loop") && addresses.Contains(instruction.Next)) labels.Add(instruction.Next);
            if (image.RelocationTargets.Contains(instruction.Address)) labels.Add(instruction.Address);
            if (index > 0 && instructions[index - 1].Next != instruction.Address) labels.Add(instruction.Address);
        }
        return labels;
    }

    private static string Reg(Register register)
    {
        var name = X86Instruction.RegisterName(register);
        var index = Array.IndexOf(Registers32, name); if (index >= 0) return $"R32({index})";
        index = Array.IndexOf(Registers16, name); if (index >= 0) return $"R16({index})";
        index = Array.IndexOf(Registers8L, name); if (index >= 0) return $"R8L({index})";
        index = Array.IndexOf(Registers8H, name); if (index >= 0) return $"R8H({index})";
        if (name.StartsWith("mm") && int.TryParse(name[2..], out index)) return $"c->mm[{index}]";
        throw new NotSupportedException("register " + name);
    }

    private static string Addr(X86Operand operand)
    {
        List<string> parts = [];
        if (operand.Base != Register.None) parts.Add(Reg(operand.Base));
        if (operand.Index != Register.None) parts.Add(Reg(operand.Index) + (operand.Scale == 1 ? "" : "*" + operand.Scale));
        if (operand.Displacement != 0 || parts.Count == 0) parts.Add($"0x{unchecked((uint)operand.Displacement):x}u");
        return "(uint32_t)(" + string.Join('+', parts) + ")";
    }

    private static string Mem(X86Operand operand, int? size = null)
    {
        if (operand.Segment == Register.FS && operand.Base == Register.None && operand.Index == Register.None && operand.Displacement == 0x2C) return "rt_tls_array(c)";
        return $"{(operand.Segment == Register.FS ? "FS" : "M")}{(size ?? operand.Size) * 8}({Addr(operand)})";
    }

    private static string Read(X86Operand operand, int? size = null)
    {
        var bytes = size ?? operand.Size;
        return operand.Kind switch
        {
            OperandKind.Register => Reg(operand.Register),
            OperandKind.Immediate => $"(({U(bytes)})0x{operand.Immediate & (bytes == 8 ? ulong.MaxValue : (1UL << bytes * 8) - 1):x}u)",
            _ => Mem(operand, bytes)
        };
    }

    private static string Write(X86Operand operand, string value) => operand.Kind == OperandKind.Register ? $"{Reg(operand.Register)} = {value};" :
        operand.Segment == Register.FS ? $"FS{operand.Size * 8}_W({Addr(operand)}, {value});" : $"{Mem(operand)} = {value};";

    private void ResultFlags(string result, int size)
    {
        E($"ZF = ({U(size)}){result} == 0;"); E($"SF = (({U(size)}){result} & {Msb(size)}) != 0;"); E($"PF = PARITY((uint8_t){result});");
    }

    private string Jump(uint target, string ret = "ret") => addresses.Contains(target) ? $"goto L_{target:x8};" : $"{{ rt_jump(c, 0x{target:x8}u, {ret}); EXIT; }}";

    public string Generate()
    {
        Labels = FindLabels(); output.Clear(); Callees.Clear(); Unsupported.Clear();
        output.AppendLine($"/* {function.Name.Replace("*/", "")} */");
        output.AppendLine($"void f_{function.Entry:x8}(CPU* RT_RESTRICT c, uint32_t entry, uint32_t ret) {{");
        E("uint32_t tgt = entry, t, a, b, r; uint64_t q; int ind = 0; (void)t; (void)a; (void)b; (void)r; (void)q;");
        E("RT_TRACE(c, entry, ret);");
        if (UsesSeh)
        {
            E($"RtAct act; act.fn = f_{function.Entry:x8}; act.guest_esp = c->esp; act.host_esp = c->host_esp;");
            E("if (rt_setjmp(act.jb)) { tgt = c->lj_target; ind = 0; rt_act_push(c, &act); goto dispatch; }");
            E("rt_act_push(c, &act);"); output.AppendLine("#define EXIT do { rt_act_pop(c, &act); return; } while (0)");
        }
        else output.AppendLine("#define EXIT return");
        output.AppendLine("dispatch:"); E("switch (tgt) {");
        foreach (var label in Labels.Order()) E($"case 0x{label:x8}u: goto L_{label:x8};");
        E("default: if (ind) { rt_jump_ind(c, tgt, ret, ind); EXIT; } c->pend = 1; c->pend_target = tgt; EXIT;"); E("}");
        for (var index = 0; index < instructions.Count; index++)
        {
            var instruction = instructions[index];
            if (Labels.Contains(instruction.Address)) output.AppendLine($"L_{instruction.Address:x8}:");
            try { Lift(instruction); }
            catch (NotSupportedException error)
            {
                Unsupported.Add($"{instruction.Address:x8}: {instruction.Text}: {error.Message}");
                var text = instruction.Text.Replace("\\", "\\\\").Replace("\"", "\\\"");
                E($"rt_unimpl(c, 0x{instruction.Address:x8}u, \"{text}\");");
            }
            if ((index + 1 >= instructions.Count || instructions[index + 1].Address != instruction.Next) && instruction.Mnemonic is not ("jmp" or "ret"))
                E($"{{ rt_jump(c, 0x{instruction.Next:x8}u, ret); EXIT; }}");
        }
        output.AppendLine("#undef EXIT"); output.AppendLine("}");
        return output.ToString();
    }

    public string GenerateSnippet(string name)
    {
        output.Clear();
        output.AppendLine($"static void {name}(CPU* c) {{");
        E("uint32_t tgt = 0, t, a, b, r, ret = 0; uint64_t q; int ind = 0;");
        E("(void)tgt; (void)t; (void)a; (void)b; (void)r; (void)q; (void)ind; (void)ret;");
        foreach (var instruction in instructions) Lift(instruction);
        output.AppendLine("}");
        return output.ToString();
    }
}
