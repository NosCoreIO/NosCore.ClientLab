using System.Buffers.Binary;
using System.Text;
using System.Text.Json.Nodes;

internal static class DiscoveryChecks
{
    public static void Run()
    {
        var nested = NosCore.ClientLab.Recompiler.FunctionDiscovery.Coverage([
            new(0x1000, "outer", [new(0x1000, 0x1100)]), new(0x1050, "inner", [new(0x1050, 0x1060)])]);
        if (!nested(0x1070) || nested(0x1100)) throw new Exception("Overlapping function ranges corrupted coverage lookup.");
        var overlapping = new NosCore.ClientLab.Recompiler.ExecutableImage(Convert.FromHexString("B840C30000C3"));
        var alternate = new NosCore.ClientLab.Recompiler.CEmitter(overlapping, new(0x1001, "overlapping entry", [new(0x1000, 0x1006)]));
        alternate.Generate();
        if (!alternate.Labels.Contains(0x1001) || !alternate.Instructions.Any(instruction => instruction.Address == 0x1001 && instruction.Mnemonic == "inc"))
            throw new Exception("Merging byte ranges hid an independently reachable x86 entry.");
        var loopImage = new NosCore.ClientLab.Recompiler.ExecutableImage(Convert.FromHexString("E00190C3"));
        var loop = new NosCore.ClientLab.Recompiler.CEmitter(loopImage, new(0x1000, "conditional loop", [new(0x1000, 0x1004)]));
        loop.Generate();
        if (!loop.Labels.Contains(0x1003)) throw new Exception("Conditional loop destination was not exported.");
        var initializerBytes = Enumerable.Repeat((byte)0x90, 0x4A).ToArray();
        Convert.FromHexString("832D00200000017340").CopyTo(initializerBytes, 0);
        initializerBytes[0x49] = 0xC3;
        var initializer = new NosCore.ClientLab.Recompiler.ExecutableImage(initializerBytes);
        initializer.RelocationTargets.Add(0x1000);
        if (!new NosCore.ClientLab.Recompiler.FunctionDiscovery(initializer).RelocationSeeds([]).Contains(0x1000))
            throw new Exception("Long Delphi unit initializer was discarded by short-routine heuristics.");
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-discovery-check-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            // A tiny original PE fixture: eight functions linked by calls. There are no recovered
            // symbols or relocations; discovery must follow more than the previous three passes.
            var bytes = new byte[0x400];
            void U16(int offset, ushort value) => BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(offset), value);
            void U32(int offset, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset), value);
            U16(0, 0x5A4D); U32(0x3C, 0x80); U32(0x80, 0x4550);
            U16(0x84, 0x14C); U16(0x86, 1); U16(0x94, 0xE0); U16(0x96, 0x102);
            const int optional = 0x98;
            U16(optional, 0x10B); U32(optional + 4, 0x200); U32(optional + 16, 0x1000);
            U32(optional + 20, 0x1000); U32(optional + 28, 0x400000);
            U32(optional + 32, 0x1000); U32(optional + 36, 0x200); U16(optional + 40, 4);
            U16(optional + 48, 4); U32(optional + 56, 0x2000); U32(optional + 60, 0x200);
            U16(optional + 68, 3); U32(optional + 92, 16);
            const int section = 0x178;
            Encoding.ASCII.GetBytes(".text").CopyTo(bytes, section);
            U32(section + 8, 0x80); U32(section + 12, 0x1000); U32(section + 16, 0x200);
            U32(section + 20, 0x200); U32(section + 36, 0x60000020);
            Array.Fill(bytes, (byte)0xCC, 0x200, 0x200);
            for (var index = 0; index < 7; index++)
            {
                var offset = 0x200 + index * 16;
                bytes[offset] = 0xE8; U32(offset + 1, 11); bytes[offset + 5] = 0xC3;
            }
            bytes[0x270] = 0xC3;
            var image = Path.Combine(root, "fixture.exe"); File.WriteAllBytes(image, bytes);
            var functions = Path.Combine(root, "functions.txt"); File.WriteAllText(functions, "");
            var seeds = Path.Combine(root, "seeds.txt"); File.WriteAllText(seeds, "00401000\n");
            var output = Path.Combine(root, "generated");
            if (NosCore.ClientLab.Recompiler.Program.Main([image, functions, output, "--seeds", seeds]) != 0)
                throw new Exception("Transitive call discovery failed to generate the fixture.");
            var report = JsonNode.Parse(File.ReadAllText(Path.Combine(output, "recompiler-report.json")))!;
            if (report["functionCount"]!.GetValue<int>() != 8) throw new Exception("Discovery stopped before following the complete call chain.");
            Console.WriteLine("PASS: function discovery follows call chains beyond three passes to completion.");
            // A called entry shares a tail already owned by another function. The split stub
            // must export the fallthrough instruction even though it is not a call/branch target.
            Array.Fill(bytes, (byte)0xCC, 0x200, 0x200);
            bytes[0x200] = 0xE8; U32(0x201, 11); bytes[0x205] = 0xC3;
            bytes[0x20F] = 0x90;
            bytes[0x210] = 0xB9; U32(0x211, 100);
            bytes[0x215] = 0xB2; bytes[0x216] = 1; bytes[0x217] = 0xC3;
            File.WriteAllBytes(image, bytes);
            File.WriteAllText(functions, "0040100f owner 0040100f-00401018\n00401010 split 00401010-00401015\n");
            var splitOutput = Path.Combine(root, "split-generated");
            if (NosCore.ClientLab.Recompiler.Program.Main([image, functions, splitOutput, "--seeds", seeds]) != 0 ||
                !File.ReadAllText(Path.Combine(splitOutput, "table.c")).Contains("{0x00401015u,"))
                throw new Exception("Split-function fallthrough has no runtime dispatch label.");
            Console.WriteLine("PASS: shared-tail fallthrough destinations are exported and known control-flow destinations are validated.");
            // Ghidra can attach a disconnected body to an existing function. It has no
            // direct call or relocation seed here, so preserving only entry points loses it.
            Array.Fill(bytes, (byte)0xCC, 0x200, 0x200);
            bytes[0x200] = 0xC3;
            bytes[0x260] = 0xB0; bytes[0x261] = 7; bytes[0x262] = 0xC3;
            File.WriteAllBytes(image, bytes);
            File.WriteAllText(functions, "00401000 separated 00401000-00401001,00401060-00401063\n");
            var rangesOutput = Path.Combine(root, "ranges-generated");
            if (NosCore.ClientLab.Recompiler.Program.Main([image, functions, rangesOutput, "--seeds", seeds]) != 0 ||
                !File.ReadAllText(Path.Combine(rangesOutput, "table.c")).Contains("{0x00401060u,"))
                throw new Exception("A disconnected Ghidra function body was lost during recompilation.");
            Console.WriteLine("PASS: full Ghidra ranges retain disconnected function bodies without call or relocation seeds.");
        }
        finally { Directory.Delete(root, recursive: true); }
    }
}
