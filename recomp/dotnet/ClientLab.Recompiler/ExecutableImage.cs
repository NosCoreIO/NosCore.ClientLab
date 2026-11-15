using System.Buffers.Binary;
using System.Reflection.PortableExecutable;
using Iced.Intel;

namespace NosCore.ClientLab.Recompiler;

public sealed class ExecutableImage
{
    private readonly byte[] file;
    private readonly SectionHeader[] sections;
    public uint ImageBase { get; }
    public uint EntryPoint { get; }
    public uint CodeStart { get; }
    public uint CodeEnd => CodeStart + (uint)Code.Length;
    public byte[] Code { get; }
    public Dictionary<uint, uint> RelocationSlots { get; } = [];
    public HashSet<uint> RelocationTargets { get; } = [];
    private readonly Dictionary<uint, X86Instruction?> decoded = [];

    public ExecutableImage(byte[] code, uint codeStart = 0x1000)
    {
        file = []; sections = []; ImageBase = codeStart; EntryPoint = codeStart; CodeStart = codeStart; Code = code.ToArray();
    }

    public ExecutableImage(string path)
    {
        file = File.ReadAllBytes(path);
        using var reader = new PEReader(new MemoryStream(file));
        var header = reader.PEHeaders.PEHeader ?? throw new InvalidDataException("Not a Windows PE executable");
        if (reader.PEHeaders.CoffHeader.Machine != Machine.I386 || header.Magic != PEMagic.PE32)
            throw new InvalidDataException("Only 32-bit x86 PE images are supported");
        ImageBase = checked((uint)header.ImageBase);
        EntryPoint = ImageBase + (uint)header.AddressOfEntryPoint;
        sections = reader.PEHeaders.SectionHeaders.ToArray();
        var code = sections.First(s => (s.SectionCharacteristics & SectionCharacteristics.ContainsCode) != 0);
        CodeStart = ImageBase + (uint)code.VirtualAddress;
        Code = file.AsSpan(code.PointerToRawData, Math.Min(code.VirtualSize, code.SizeOfRawData)).ToArray();
        var directory = header.BaseRelocationTableDirectory;
        if (directory.Size == 0) return;
        var position = Offset((uint)directory.RelativeVirtualAddress);
        var end = Math.Min(file.Length, position + directory.Size);
        while (position + 8 <= end)
        {
            var page = Read32(position);
            var size = Read32(position + 4);
            if (size < 8 || size > end - position) break;
            for (var slot = position + 8; slot + 2 <= position + size; slot += 2)
            {
                var entry = BinaryPrimitives.ReadUInt16LittleEndian(file.AsSpan(slot, 2));
                if (entry >> 12 != 3) continue;
                var rva = page + (uint)(entry & 0xFFF);
                var value = Read32(Offset(rva));
                RelocationSlots[ImageBase + rva] = value;
                if (ContainsCode(value)) RelocationTargets.Add(value);
            }
            position += (int)size;
        }
    }

    private uint Read32(int offset) => BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(offset, 4));
    private int Offset(uint rva)
    {
        foreach (var section in sections)
            if (rva >= section.VirtualAddress && rva < section.VirtualAddress + Math.Max(section.VirtualSize, section.SizeOfRawData))
                return checked(section.PointerToRawData + (int)rva - section.VirtualAddress);
        if (rva < file.Length) return (int)rva;
        throw new InvalidDataException("PE address outside image");
    }

    public bool ContainsCode(uint address) => address >= CodeStart && address < CodeEnd;
    public X86Instruction? Decode(uint address)
    {
        if (!ContainsCode(address)) return null;
        if (decoded.TryGetValue(address, out var cached)) return cached;
        var decoder = Decoder.Create(32, new ByteArrayCodeReader(Code, (int)(address - CodeStart), Math.Min(15, (int)(CodeEnd - address))));
        decoder.IP = address;
        decoder.Decode(out var instruction);
        var result = instruction.Code == Iced.Intel.Code.INVALID ? null : new X86Instruction(instruction);
        decoded[address] = result;
        return result;
    }

    public List<X86Instruction> Disassemble(uint start, uint end)
    {
        List<X86Instruction> result = [];
        for (var address = start; address < end;)
        {
            var instruction = Decode(address);
            if (instruction is null) break;
            result.Add(instruction);
            address = instruction.Next;
        }
        return result;
    }

    public IEnumerable<uint> JumpTable(X86Instruction instruction)
    {
        if (instruction.Mnemonic != "jmp" || instruction.Operands.FirstOrDefault() is not { Kind: OperandKind.Memory } operand ||
            operand.Base != Register.None || operand.Index == Register.None || operand.Scale != 4) yield break;
        var slot = (uint)operand.Displacement;
        for (var count = 0; count < 1024 && RelocationSlots.TryGetValue(slot, out var target) && ContainsCode(target); count++, slot += 4)
            yield return target;
    }
}
