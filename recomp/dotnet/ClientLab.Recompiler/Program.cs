using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace NosCore.ClientLab.Recompiler;

public static class Program
{
    // Change only when generated/runtime semantics require rebuilding existing clients.
    public const int RuntimeRevision = 19;
    public static int Main(string[] args)
    {
        try
        {
            if (args.Length == 3 && args[0] == "--emit-json")
            {
                var cases = JsonSerializer.Deserialize<SnippetCase[]>(File.ReadAllText(args[1]))!;
                Dictionary<string, string> snippets = [];
                foreach (var item in cases)
                {
                    var image = new ExecutableImage(Convert.FromHexString(item.Code), item.Address);
                    var emitter = new CEmitter(image, new(item.Address, item.Name, [new(item.Address, image.CodeEnd)]));
                    try { snippets[item.Name] = emitter.GenerateSnippet(item.Name); }
                    catch (NotSupportedException) { snippets[item.Name] = $"static void {item.Name}(CPU* c) {{ rt_unimpl(c, 0, \"unsupported instruction\"); }}"; }
                }
                File.WriteAllText(args[2], JsonSerializer.Serialize(snippets));
                return 0;
            }
            if (args.Length >= 2 && args[0] == "--emit-bytes")
            {
                var image = new ExecutableImage(Convert.FromHexString(args[1]));
                var emitter = new CEmitter(image, new(0x1000, "instruction_test", [new(0x1000, image.CodeEnd)]));
                Console.Write(emitter.Generate());
                return emitter.Unsupported.Count > 0 ? 2 : 0;
            }
            if (args.Length < 3)
            {
                Console.Error.WriteLine("Usage: ClientLab.Recompiler <client.exe> <functions.txt> <output-dir> [--seeds file] [--allow-unsupported]");
                return 1;
            }
            var source = Path.GetFullPath(args[0]); var output = Path.GetFullPath(args[2]);
            var imageFile = new ExecutableImage(source); var discovery = new FunctionDiscovery(imageFile);
            var functions = FunctionDiscovery.ReadFunctions(args[1]).Select(function => function with
            {
                Ranges = function.Ranges.Select(range => new AddressRange(Math.Max(range.Start, imageFile.CodeStart), Math.Min(range.End, imageFile.CodeEnd))).Where(range => range.Start < range.End).ToList()
            }).Where(function => function.Ranges.Count > 0 && imageFile.ContainsCode(function.Entry)).ToList();
            functions = discovery.Extend(functions);
            functions.AddRange(discovery.Discover(functions, discovery.RelocationSeeds(functions)));
            var seedsIndex = Array.IndexOf(args, "--seeds");
            if (seedsIndex >= 0 && seedsIndex + 1 < args.Length)
            {
                var seeds = File.ReadLines(args[seedsIndex + 1]).Where(line => !string.IsNullOrWhiteSpace(line) && !line.StartsWith('#')).Select(line => FunctionDiscovery.Hex(line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries)[0]));
                functions.AddRange(discovery.Discover(functions, seeds));
            }
            (functions, var extraLabels) = discovery.AbsorbGaps(functions);
            // Fresh client builds have no Ghidra function list. Follow call chains to a fixed point,
            // rather than stopping after three levels and silently omitting reachable functions.
            while (true)
            {
                functions = functions.OrderBy(f => f.Entry).ToList();
                var covered = FunctionDiscovery.Coverage(functions);
                Dictionary<uint, uint> owners = []; HashSet<uint> calls = [];
                foreach (var function in functions)
                {
                    var emitter = new CEmitter(imageFile, function, covered);
                    var addresses = emitter.Instructions.Select(i => i.Address).ToHashSet();
                    for (var index = 0; index < emitter.Instructions.Count; index++)
                    {
                        var instruction = emitter.Instructions[index];
                        owners.TryAdd(instruction.Address, function.Entry);
                        if ((instruction.Mnemonic == "call" || instruction.Mnemonic.StartsWith('j') || instruction.Mnemonic.StartsWith("loop")) && instruction.DirectTarget is { } target &&
                            (instruction.Mnemonic == "call" || !addresses.Contains(target))) calls.Add(target);
                        // Discovery can split a shared tail between functions. The emitter transfers
                        // such fallthroughs through rt_jump, so their destinations need exported labels.
                        if (instruction.Mnemonic is not ("jmp" or "ret" or "int3") &&
                            (index + 1 == emitter.Instructions.Count || emitter.Instructions[index + 1].Address != instruction.Next))
                            calls.Add(instruction.Next);
                        foreach (var destination in imageFile.JumpTable(instruction)) calls.Add(destination);
                        if (instruction.Mnemonic == "push" && instruction.DirectTarget is { } handler && imageFile.ContainsCode(handler) && imageFile.Decode(instruction.Next)?.IsFsPush == true)
                            foreach (var entry in discovery.HandlerEntries(handler)) calls.Add(entry);
                    }
                }
                var entries = functions.Select(f => f.Entry).ToHashSet(); HashSet<uint> seeds = [];
                foreach (var target in calls.Concat(imageFile.RelocationTargets))
                {
                    if (entries.Contains(target) || !imageFile.ContainsCode(target)) continue;
                    if (owners.TryGetValue(target, out var owner))
                    {
                        if (!extraLabels.TryGetValue(owner, out var labels)) extraLabels[owner] = labels = [];
                        labels.Add(target);
                    }
                    else if (calls.Contains(target)) seeds.Add(target);
                }
                Console.WriteLine($"{extraLabels.Values.Sum(set => set.Count)} mid-function labels, {seeds.Count} uncovered call targets");
                if (seeds.Count == 0) break;
                var discovered = discovery.Discover(functions, seeds.Order());
                if (discovered.Count == 0) break;
                functions.AddRange(discovered);
            }
            Directory.CreateDirectory(output);
            var finalCoverage = FunctionDiscovery.Coverage(functions);
            var callOwners = functions.ToDictionary(f => f.Entry, f => f.Entry);
            foreach (var (owner, labels) in extraLabels)
                foreach (var label in labels) callOwners.TryAdd(label, owner);
            Dictionary<uint, List<string>> chunks = []; Dictionary<uint, HashSet<uint>> callees = [];
            HashSet<(uint Label, uint Owner)> table = []; List<string> unsupported = [];
            HashSet<uint> requiredTargets = [];
            var lifted = new byte[imageFile.Code.Length];
            foreach (var function in functions.OrderBy(f => f.Entry))
            {
                var emitter = new CEmitter(imageFile, function, finalCoverage)
                { ExtraLabels = extraLabels.GetValueOrDefault(function.Entry) ?? [], CallOwners = callOwners };
                var text = emitter.Generate(); var key = function.Entry >> 15;
                if (!chunks.TryGetValue(key, out var values)) { chunks[key] = values = []; callees[key] = []; }
                values.Add(text); callees[key].UnionWith(emitter.Callees); unsupported.AddRange(emitter.Unsupported);
                foreach (var label in emitter.Labels) table.Add((label, function.Entry));
                for (var index = 0; index < emitter.Instructions.Count; index++)
                {
                    var instruction = emitter.Instructions[index];
                    if ((instruction.Mnemonic == "call" || instruction.Mnemonic.StartsWith('j') || instruction.Mnemonic.StartsWith("loop")) && instruction.DirectTarget is { } target && imageFile.ContainsCode(target)) requiredTargets.Add(target);
                    if (instruction.Mnemonic is not ("jmp" or "ret" or "int3") &&
                        (index + 1 == emitter.Instructions.Count || emitter.Instructions[index + 1].Address != instruction.Next) && imageFile.ContainsCode(instruction.Next)) requiredTargets.Add(instruction.Next);
                    requiredTargets.UnionWith(imageFile.JumpTable(instruction));
                    if (instruction.Address >= imageFile.CodeStart && instruction.Next <= imageFile.CodeEnd)
                        Array.Fill(lifted, (byte)1, (int)(instruction.Address - imageFile.CodeStart), instruction.Length);
                }
            }
            var exported = table.Select(pair => pair.Label).ToHashSet();
            // Speculative relocation discovery can decode embedded data until an invalid x86
            // byte sequence. Preserve the real illegal-instruction behavior if it is reached;
            // do not mistake it for valid code or silently allow a missing dispatch target.
            var invalidTargets = requiredTargets.Except(exported).Where(target => imageFile.Decode(target) is null).Order().ToArray();
            foreach (var target in invalidTargets)
            {
                var key = target >> 15;
                if (!chunks.TryGetValue(key, out var values)) { chunks[key] = values = []; callees[key] = []; }
                values.Add($"void f_{target:x8}(CPU* RT_RESTRICT c, uint32_t entry, uint32_t ret) {{ rt_unimpl(c, 0x{target:x8}u, \"invalid x86 instruction\"); }}\n");
                table.Add((target, target)); exported.Add(target);
                unsupported.Add($"{target:x8}: invalid x86 instruction reached by speculative discovery");
            }
            var missingTargets = requiredTargets.Except(exported).Order().ToArray();
            if (missingTargets.Length > 0)
            {
                File.WriteAllText(Path.Combine(output, "control-flow-errors.json"), JsonSerializer.Serialize(new {
                    targets = missingTargets, functions, extraLabels
                }));
                throw new InvalidDataException("Known control-flow targets have no compiled dispatch label: " + string.Join(", ", missingTargets.Take(20).Select(address => address.ToString("x8"))));
            }
            Console.WriteLine($"Validated {requiredTargets.Count} known control-flow destinations.");
            var wanted = new HashSet<string>();
            foreach (var (key, values) in chunks)
            {
                var filename = $"fn_{key << 15:x8}.c"; wanted.Add(filename);
                var declarations = string.Join("", callees[key].Order().Select(target => $"void f_{target:x8}(CPU*, uint32_t, uint32_t);\n"));
                WriteIfChanged(Path.Combine(output, filename), "#include \"rt.h\"\n\n" + declarations + "\n" + string.Join("\n\n", values));
            }
            foreach (var old in Directory.EnumerateFiles(output, "fn_*.c"))
                if (!wanted.Contains(Path.GetFileName(old))) File.Delete(old);
            var labelsTable = new StringBuilder("#include \"rt.h\"\n\n");
            foreach (var owner in table.Select(pair => pair.Owner).Distinct().Order()) labelsTable.AppendLine($"void f_{owner:x8}(CPU*, uint32_t, uint32_t);");
            labelsTable.AppendLine($"\nconst uint32_t rt_label_count = {table.Count};\nconst RtLabel rt_labels[] = {{");
            foreach (var (label, owner) in table.OrderBy(pair => pair.Label).ThenBy(pair => pair.Label != pair.Owner).ThenBy(pair => pair.Owner))
                labelsTable.AppendLine($"  {{0x{label:x8}u, f_{owner:x8}, {(label == owner ? 1 : 0)}}},");
            labelsTable.AppendLine("};"); labelsTable.AppendLine($"const uint32_t rt_code_lo = 0x{imageFile.CodeStart:x8}u, rt_code_hi = 0x{imageFile.CodeEnd:x8}u;");
            WriteIfChanged(Path.Combine(output, "table.c"), labelsTable.ToString());
            var percentage = lifted.Sum(value => (long)value) * 100d / lifted.Length;
            var report = new { schema = 1, runtimeRevision = RuntimeRevision, validatedTargets = requiredTargets.Count, imageSha256 = Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(source))), entryPoint = imageFile.EntryPoint,
                functionCount = functions.Count, labelCount = table.Count, coveragePercent = percentage, invalidCodeTargets = invalidTargets.Select(target => target.ToString("x8")).ToArray(), unsupported };
            WriteIfChanged(Path.Combine(output, "recompiler-report.json"), JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine($"{functions.Count} functions, {table.Count} labels, {chunks.Count} files; coverage {percentage:F1}%; {unsupported.Count} unsupported instructions");
            foreach (var line in unsupported.Take(15)) Console.Error.WriteLine(line);
            return unsupported.Count > 0 && !args.Contains("--allow-unsupported") ? 2 : 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error.Message); return 1; }
    }

    private sealed record SnippetCase(string Code, string Name, uint Address);

    private static void WriteIfChanged(string path, string text)
    {
        if (File.Exists(path) && File.ReadAllText(path) == text) return;
        File.WriteAllText(path, text, new UTF8Encoding(false));
    }
}
