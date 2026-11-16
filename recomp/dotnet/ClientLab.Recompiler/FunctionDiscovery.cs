using System.Globalization;
using Iced.Intel;

namespace NosCore.ClientLab.Recompiler;

public readonly record struct AddressRange(uint Start, uint End);
public sealed record FunctionDefinition(uint Entry, string Name, List<AddressRange> Ranges);

public sealed class FunctionDiscovery(ExecutableImage image)
{
    private static readonly HashSet<string> Conditions = ["je", "jne", "jb", "jae", "jbe", "ja", "jl", "jge", "jle", "jg", "js", "jns", "jo", "jno", "jp", "jnp"];
    private static readonly HashSet<string> Sane = ["mov", "push", "pop", "add", "sub", "and", "or", "xor", "cmp", "test", "lea", "call", "jmp", "ret", "inc", "dec", "movzx", "movsx", "shl", "shr", "sar", "imul", "neg", "not", "xchg", "cdq", "nop", "leave", "fld", "fstp", "fild", "wait", "sete", "setne"];
    private static readonly HashSet<string> Junk = ["in", "out", "insb", "insd", "outsb", "outsd", "hlt", "arpl", "das", "aaa", "aas", "daa", "bound", "les", "lds", "into", "sldt", "callf", "jmpf", "retf", "iretd", "int1", "salc", "aam", "aad"];

    public static List<FunctionDefinition> ReadFunctions(string filename)
    {
        List<FunctionDefinition> functions = [];
        foreach (var line in File.ReadLines(filename))
        {
            var parts = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 3 || parts[0].StartsWith('#')) continue;
            var ranges = parts[2].Split(',').Select(value => value.Split('-')).Select(pair => new AddressRange(Hex(pair[0]), Hex(pair[1]))).ToList();
            functions.Add(new(Hex(parts[0]), parts[1], ranges));
        }
        return functions.OrderBy(f => f.Entry).ToList();
    }

    public static uint Hex(string value) => uint.Parse(value.StartsWith("0x") ? value[2..] : value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);

    public static Func<uint, bool> Coverage(IEnumerable<FunctionDefinition> functions)
    {
        var ranges = ToRanges(functions.SelectMany(f => f.Ranges).Select(r => new KeyValuePair<uint, uint>(r.Start, r.End))).ToArray();
        return address =>
        {
            var lo = 0; var hi = ranges.Length;
            while (lo < hi) { var middle = (lo + hi) / 2; if (ranges[middle].Start <= address) lo = middle + 1; else hi = middle; }
            return lo > 0 && address < ranges[lo - 1].End;
        };
    }

    public List<uint> HandlerEntries(uint handler) => image.ContainsCode(handler) && image.Code[handler - image.CodeStart] == 0xE9 ? [handler, handler + 5] : [handler];

    public Dictionary<uint, uint> Descend(IEnumerable<uint> starts, Func<uint, bool> mayEnter)
    {
        Dictionary<uint, uint> seen = [];
        var work = new Stack<uint>(starts);
        while (work.TryPop(out var address))
        {
            while (!seen.ContainsKey(address) && image.ContainsCode(address) && mayEnter(address))
            {
                var instruction = image.Decode(address);
                if (instruction is null) break;
                seen[address] = instruction.Next;
                var mnemonic = instruction.Mnemonic;
                if ((mnemonic.StartsWith('j') || mnemonic.StartsWith("loop")) && instruction.DirectTarget is { } target) work.Push(target);
                foreach (var jump in image.JumpTable(instruction)) work.Push(jump);
                if (mnemonic == "push" && instruction.DirectTarget is { } handler && image.ContainsCode(handler) && image.Decode(instruction.Next)?.IsFsPush == true)
                    foreach (var entry in HandlerEntries(handler)) work.Push(entry);
                if (mnemonic is "jmp" or "ret" or "int3") break;
                address = instruction.Next;
            }
        }
        return seen;
    }

    public static List<AddressRange> ToRanges(IEnumerable<KeyValuePair<uint, uint>> seen)
    {
        List<AddressRange> ranges = [];
        foreach (var pair in seen.OrderBy(p => p.Key))
        {
            if (ranges.Count > 0 && ranges[^1].End >= pair.Key)
                ranges[^1] = new(ranges[^1].Start, Math.Max(ranges[^1].End, pair.Value));
            else ranges.Add(new(pair.Key, pair.Value));
        }
        return ranges;
    }

    public List<FunctionDefinition> Extend(List<FunctionDefinition> functions)
    {
        var covered = Coverage(functions);
        var entries = functions.Select(f => f.Entry).ToHashSet();
        List<FunctionDefinition> result = [];
        var grown = 0;
        foreach (var function in functions)
        {
            bool Mine(uint address) => function.Ranges.Any(range => range.Start <= address && address < range.End);
            var seen = Descend(function.Ranges.Select(r => r.Start), address => Mine(address) || !covered(address) && !entries.Contains(address));
            var extra = seen.Where(p => !Mine(p.Key)).ToDictionary();
            if (extra.Count > 0)
            {
                grown++;
                foreach (var range in function.Ranges) extra[range.Start] = range.End;
                result.Add(function with { Ranges = ToRanges(extra) });
            }
            else result.Add(function);
        }
        Console.WriteLine($"{grown} functions extended past their Ghidra ranges");
        return result;
    }

    public List<FunctionDefinition> Discover(List<FunctionDefinition> functions, IEnumerable<uint> seeds)
    {
        var covered = Coverage(functions);
        var entries = functions.Select(f => f.Entry).ToHashSet();
        List<FunctionDefinition> result = [];
        foreach (var seed in seeds)
        {
            if (entries.Contains(seed) || !image.ContainsCode(seed)) continue;
            var ranges = ToRanges(Descend([seed], address => address == seed || !covered(address)));
            if (ranges.Count == 0) continue;
            result.Add(new(seed, $"seed_{seed:x8}", ranges)); entries.Add(seed);
        }
        Console.WriteLine($"{result.Count} functions discovered from seeds");
        return result;
    }

    private static bool IsSane(string mnemonic) => Sane.Contains(mnemonic) || Conditions.Contains(mnemonic);

    public List<uint> RelocationSeeds(List<FunctionDefinition> functions)
    {
        var covered = Coverage(functions);
        bool Shape(uint target)
        {
            if (target % 4 == 0) return true;
            var instructions = image.Disassemble(target, target + 8);
            if (instructions.Count < 2) return false;
            var first = instructions[0]; var second = instructions[1];
            if (first.Mnemonic == "add" && second.Mnemonic == "jmp" && first.Operands[0] is { } operand &&
                (operand.Register == Register.EAX || operand.Kind == OperandKind.Memory && operand.Base == Register.ESP && operand.Displacement == 4)) return true;
            if (first.Mnemonic == "push" && first.Operands[0].Register == Register.EBP && second.Mnemonic == "mov" && second.Operands is [{ Register: Register.EBP }, { Register: Register.ESP }]) return true;
            if (first.Mnemonic == "push" && first.Operands[0].Register is Register.EBX or Register.ESI or Register.EDI && second.Mnemonic is "push" or "mov" or "add" or "sub" or "xor") return true;
            // Delphi leaf methods can start with operations on argument registers, including
            // memory operands based on them. Inspect operands rather than letters in the mnemonic.
            static bool ArgumentRegister(Register register) => register is Register.EAX or Register.EDX or Register.ECX or Register.AL or Register.AX;
            if ((IsSane(first.Mnemonic) || first.Mnemonic.StartsWith('f')) &&
                first.Operands.Any(operand => operand.Kind == OperandKind.Register && ArgumentRegister(operand.Register) ||
                    operand.Kind == OperandKind.Memory && (ArgumentRegister(operand.Base) || ArgumentRegister(operand.Index))))
            {
                var address = target;
                for (var count = 0; count < 24; count++)
                {
                    var instruction = image.Decode(address);
                    if (instruction is null || !IsSane(instruction.Mnemonic) && !instruction.Mnemonic.StartsWith('f')) return false;
                    if (instruction.Mnemonic == "ret") return true;
                    if (instruction.Mnemonic == "jmp") return false;
                    address = instruction.Next;
                }
            }
            return false;
        }
        HashSet<uint> stored = [];
        foreach (var (slot, target) in image.RelocationSlots)
        {
            if (!image.ContainsCode(slot) || !image.ContainsCode(target) || covered(target)) continue;
            for (uint distance = 2; distance < 11; distance++)
            {
                var instruction = image.Decode(slot - distance);
                if (instruction?.Next == slot + 4 && instruction.Mnemonic == "mov" && instruction.Operands.Length == 2 &&
                    instruction.Operands[0].Kind == OperandKind.Memory && instruction.Operands[1].Kind == OperandKind.Immediate && (uint)instruction.Operands[1].Immediate == target)
                { stored.Add(target); break; }
            }
        }
        var result = stored.Order().ToList();
        foreach (var target in image.RelocationTargets.Order())
        {
            if (stored.Contains(target) || covered(target) || !Shape(target)) continue;
            // Delphi unit initialization/finalization callbacks may be long straight-line
            // routines. Their guard skips directly to RET when the unit counter is settled;
            // requiring a terminal within the first 24 sequential instructions loses them.
            var first = image.Decode(target);
            var guard = first is null ? null : image.Decode(first.Next);
            if (first is { Mnemonic: "sub", Operands: [{ Kind: OperandKind.Memory, Base: Register.None, Index: Register.None }, { Kind: OperandKind.Immediate, Immediate: 1 }] } &&
                guard is { Mnemonic: "jae", DirectTarget: { } exit } && image.Decode(exit)?.Mnemonic == "ret")
            { result.Add(target); continue; }
            var address = target;
            for (var count = 0; count < 24; count++)
            {
                var instruction = image.Decode(address);
                if (instruction is null || !IsSane(instruction.Mnemonic)) break;
                if (instruction.Mnemonic is "jmp" or "ret") { result.Add(target); break; }
                address = instruction.Next;
            }
        }
        Console.WriteLine($"{result.Count} relocation targets look like uncovered code");
        return result;
    }

    public (List<FunctionDefinition> Functions, Dictionary<uint, HashSet<uint>> Labels) AbsorbGaps(List<FunctionDefinition> functions)
    {
        functions = functions.OrderBy(f => f.Entry).ToList();
        var spans = functions.SelectMany((function, index) => function.Ranges.Select(range => (Range: range, Owner: index))).OrderBy(s => s.Range.Start).ToArray();
        Dictionary<uint, HashSet<uint>> labels = [];
        List<(int Owner, uint Start, uint End)> additions = [];
        for (var index = 0; index + 1 < spans.Length; index++)
        {
            var start = spans[index].Range.End; var end = spans[index + 1].Range.Start;
            while (start < end && image.ContainsCode(start) && image.Code[start - image.CodeStart] is 0x00 or 0x90 or 0xCC or 0x8B or 0xC0) start++;
            if (end < start + 4 || !image.ContainsCode(start) || end > image.CodeEnd) continue;
            if (image.RelocationSlots.Keys.Count(address => start <= address && address < end - 3) * 4 >= (end - start) / 2) continue;
            var body = image.Code.AsSpan((int)(start - image.CodeStart), (int)(end - start));
            if (body.ToArray().Count(value => value is >= 32 and < 127) > .7 * body.Length || body[..Math.Min(16, body.Length)].IndexOf(new byte[] { 255, 255, 255, 255 }) >= 0) continue;
            var instructions = image.Disassemble(start, end);
            if (instructions.Count == 0 || instructions.Sum(i => i.Length) < .95 * (end - start) || instructions.Any(i => Junk.Contains(i.Mnemonic)) || !instructions.Any(i => i.Mnemonic is "ret" or "jmp")) continue;
            additions.Add((spans[index].Owner, start, instructions[^1].Next));
        }
        foreach (var (owner, start, end) in additions)
        {
            var function = functions[owner]; function.Ranges.Add(new(start, end));
            if (!labels.TryGetValue(function.Entry, out var values)) labels[function.Entry] = values = [];
            values.Add(start);
        }
        Console.WriteLine($"{additions.Count} unreached code fragments joined to preceding functions");
        return (functions, labels);
    }
}
