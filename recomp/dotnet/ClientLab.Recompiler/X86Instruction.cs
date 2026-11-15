using Iced.Intel;

namespace NosCore.ClientLab.Recompiler;

public enum OperandKind { Register, Immediate, Memory }

public sealed record X86Operand(OperandKind Kind, int Size, Register Register = Register.None,
    ulong Immediate = 0, Register Base = Register.None, Register Index = Register.None,
    int Scale = 1, long Displacement = 0, Register Segment = Register.None);

public sealed class X86Instruction
{
    public uint Address { get; }
    public int Length { get; }
    public uint Next => Address + (uint)Length;
    public string Mnemonic { get; }
    public string Text { get; }
    public X86Operand[] Operands { get; }
    public uint? DirectTarget => Operands.FirstOrDefault() is { Kind: OperandKind.Immediate } operand ? (uint)operand.Immediate : null;
    public bool IsFsPush => Mnemonic == "push" && Operands.FirstOrDefault() is { Kind: OperandKind.Memory, Segment: Register.FS };
    public bool WritesFsEsp => Mnemonic == "mov" && Operands.Length == 2 && Operands[0] is { Kind: OperandKind.Memory, Segment: Register.FS } && Operands[1].Register == Register.ESP;

    public X86Instruction(Instruction instruction)
    {
        Address = (uint)instruction.IP;
        Length = instruction.Length;
        var mnemonic = instruction.Mnemonic.ToString().ToLowerInvariant();
        if (instruction.HasLockPrefix) mnemonic = "lock " + mnemonic;
        else if (instruction.HasRepnePrefix) mnemonic = "repne " + mnemonic;
        else if (instruction.HasRepPrefix)
            mnemonic = (mnemonic.StartsWith("cmps") || mnemonic.StartsWith("scas") ? "repe " : "rep ") + mnemonic;
        Mnemonic = mnemonic;
        var formatter = new IntelFormatter();
        var output = new StringOutput();
        formatter.Format(instruction, output);
        Text = output.Value;
        Operands = Enumerable.Range(0, instruction.OpCount).Select(index => ConvertOperand(instruction, index)).ToArray();
    }

    private static X86Operand ConvertOperand(Instruction instruction, int index)
    {
        var kind = instruction.GetOpKind(index);
        if (kind == OpKind.Register)
        {
            var register = instruction.GetOpRegister(index);
            return new(OperandKind.Register, register.GetSize(), Register: register);
        }
        if (kind is OpKind.NearBranch16 or OpKind.NearBranch32 or OpKind.NearBranch64)
            return new(OperandKind.Immediate, 4, Immediate: instruction.NearBranchTarget);
        if (kind is OpKind.Immediate8 or OpKind.Immediate8_2nd or OpKind.Immediate16 or OpKind.Immediate32 or OpKind.Immediate64 or
            OpKind.Immediate8to16 or OpKind.Immediate8to32 or OpKind.Immediate8to64 or OpKind.Immediate32to64)
        {
            var size = kind switch { OpKind.Immediate8 or OpKind.Immediate8_2nd => 1, OpKind.Immediate16 or OpKind.Immediate8to16 => 2,
                OpKind.Immediate64 or OpKind.Immediate8to64 or OpKind.Immediate32to64 => 8, _ => 4 };
            return new(OperandKind.Immediate, size, Immediate: instruction.GetImmediate(index));
        }
        var memoryBase = kind switch
        {
            OpKind.MemorySegSI => Register.SI, OpKind.MemorySegESI => Register.ESI,
            OpKind.MemorySegDI or OpKind.MemoryESDI => Register.DI,
            OpKind.MemorySegEDI or OpKind.MemoryESEDI => Register.EDI, _ => instruction.MemoryBase
        };
        return new(OperandKind.Memory, instruction.MemorySize.GetSize(), Base: memoryBase, Index: instruction.MemoryIndex,
            Scale: instruction.MemoryIndexScale, Displacement: unchecked((long)instruction.MemoryDisplacement64), Segment: instruction.MemorySegment);
    }

    public static string RegisterName(Register register)
    {
        if (register is >= Register.ST0 and <= Register.ST7) return $"st({register - Register.ST0})";
        return register.ToString().ToLowerInvariant();
    }

    private sealed class StringOutput : FormatterOutput
    {
        private readonly System.Text.StringBuilder builder = new();
        public string Value => builder.ToString();
        public override void Write(string text, FormatterTextKind kind) => builder.Append(text);
    }
}
