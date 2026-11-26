using System.Diagnostics;
using System.Text;
using NosCore.ClientLab.Launcher.Services;
using Wasmtime;

/// <summary>
/// <c>ClientLab.Checks --wasm-toolchain &lt;prebuilt runtime module folder&gt; &lt;data folder&gt;</c>: the real WebAssembly
/// compiler on this platform. It downloads the pinned package into the data folder (reused when present), builds a
/// small game module against the prebuilt runtime module the way the launcher does, checks the module, and cancels a
/// compile midway. Opt-in because it downloads about 100 MB; the release workflow runs it on every platform.
/// </summary>
internal static class WasiToolchainChecks
{
    public static async Task RunAsync(string bundleDirectory, string dataDirectory)
    {
        var sources = FindSources();
        var work = Path.Combine(Path.GetTempPath(), "ClientLab-wasi-check-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dataDirectory);
        await using var app = new LauncherCoordinator(sources, dataDirectory);
        app.Logged += line => Console.WriteLine("  " + line);
        try
        {
            var generated = Path.Combine(work, "generated");
            Directory.CreateDirectory(generated);
            // The shape of recompiler output: rt.h, lifted functions, and the label table the runtime module looks up.
            await File.WriteAllTextAsync(Path.Combine(generated, "fn_00401000.c"), """
                #include "rt.h"

                void fn_00401000(CPU* c, uint32_t a, uint32_t b) {
                    c->eax = c->ecx * 3u + 7u;
                    PUSH(c->eax);
                    c->esp += 4;
                }
                """);
            await File.WriteAllTextAsync(Path.Combine(generated, "table.c"), """
                #include "rt.h"

                void fn_00401000(CPU* c, uint32_t a, uint32_t b);
                const RtLabel rt_labels[] = { { 0x401000u, fn_00401000, 1u } };
                const uint32_t rt_label_count = 1;
                const uint32_t rt_code_lo = 0x401000u, rt_code_hi = 0x402000u;
                """);
            var bundle = HostBundle.FromDirectory(bundleDirectory);
            var output = Path.Combine(work, "web");
            var clock = Stopwatch.StartNew();
            // Automatic choice on a machine without Emscripten: the WebAssembly compiler, never an emsdk install.
            await WasmCompiler.BuildAsync(sources, generated, output, CompilerChoice.Auto,
                _ => throw new Exception("Emscripten was set up although the WebAssembly compiler could build."), app, default, bundle);
            Console.WriteLine($"Built a game module with the WebAssembly compiler in {clock.Elapsed.TotalSeconds:0.0} s.");
            CheckGameModule(Path.Combine(output, "nostale-game.wasm"));
            foreach (var file in WasmCompiler.HostFiles)
                Check(File.Exists(Path.Combine(output, file)), $"{file} of the prebuilt runtime module was not installed.");

            await CancellationCheckAsync(sources, work, app);

            // The toolchain still works after a cancelled run, and reuses the objects it compiled.
            clock.Restart();
            await WasmCompiler.BuildAsync(sources, generated, Path.Combine(work, "web-again"), CompilerChoice.WebAssembly,
                _ => throw new Exception("Emscripten was set up although the WebAssembly compiler was chosen."), app, default, bundle);
            CheckGameModule(Path.Combine(work, "web-again", "nostale-game.wasm"));
            Console.WriteLine($"Rebuilt from cached objects in {clock.Elapsed.TotalSeconds:0.0} s.");

            // An installed Emscripten that no longer works falls back to the WebAssembly compiler.
            await WasmCompiler.BuildAsync(sources, generated, Path.Combine(work, "web-fallback"), CompilerChoice.Auto,
                _ => Task.FromResult(Path.Combine(work, "removed-emsdk", OperatingSystem.IsWindows() ? "emcc.cmd" : "emcc")), app, default, bundle, emscriptenInstalled: true);
            CheckGameModule(Path.Combine(work, "web-fallback", "nostale-game.wasm"));
            Console.WriteLine("A broken Emscripten install fell back to the WebAssembly compiler.");
            Console.WriteLine("WebAssembly compiler checks passed.");
        }
        finally { try { Directory.Delete(work, recursive: true); } catch (IOException) { } }
    }

    /// <summary>A valid side module that exports the four symbols the runtime module links against.</summary>
    private static void CheckGameModule(string path)
    {
        var bytes = File.ReadAllBytes(path);
        using var engine = new Engine();
        Check(Module.Validate(engine, bytes) is null, "The game module is not valid WebAssembly.");
        using var module = Module.FromBytes(engine, "game", bytes);
        var exports = module.Exports.Select(export => export.Name).ToHashSet(StringComparer.Ordinal);
        foreach (var symbol in new[] { "rt_labels", "rt_label_count", "rt_code_lo", "rt_code_hi", "__wasm_call_ctors" })
            Check(exports.Contains(symbol), $"The game module does not export {symbol}.");
        Check(module.Imports.Any(import => import.ModuleName == "env" && import.Name == "memory"), "The game module does not import the runtime module's memory.");
        Check(Encoding.ASCII.GetString(bytes, 8, Math.Min(64, bytes.Length - 8)).Contains("dylink.0"), "The game module has no dynamic-linking section.");
    }

    /// <summary>Cancelling stops clang within a moment, not when the file is done.</summary>
    private static async Task CancellationCheckAsync(string sources, string work, LauncherCoordinator app)
    {
        var big = Path.Combine(work, "big");
        Directory.CreateDirectory(big);
        var code = new StringBuilder("#include <stdint.h>\n");
        for (var function = 0; function < 6000; function++)
        {
            code.Append($"uint32_t f{function}(uint32_t x, uint32_t y) {{ uint32_t s = 0; for (uint32_t i = 0; i < x; i++) {{ switch ((i ^ y) & 7) {{");
            for (var arm = 0; arm < 8; arm++) code.Append($" case {arm}: s = s * {arm + 3}u + (x >> {arm}) + f{Math.Max(0, function - 1)}(i, s); break;");
            code.Append(" } } return s; }\n");
        }
        await File.WriteAllTextAsync(Path.Combine(big, "big.c"), code.ToString());
        using var wasi = await WasiToolchain.PrepareAsync(app.DataDirectory, app, default);
        using var stop = new CancellationTokenSource();
        var run = wasi.RunAsync("clang", ["--target=wasm32-unknown-emscripten", "-O2", "-c", "/src/big.c", "-o", "/out/big.o"],
            [new(big, "/src"), new(big, "/out", Writable: true)], app, stop.Token);
        await Task.WhenAny(run, Task.Delay(TimeSpan.FromSeconds(2)));
        Check(!run.IsCompleted, "The cancellation fixture compiled too quickly to test cancelling.");
        var clock = Stopwatch.StartNew();
        stop.Cancel();
        try
        {
            await run.WaitAsync(TimeSpan.FromSeconds(10));
            throw new Exception("A cancelled compile finished instead of stopping.");
        }
        catch (OperationCanceledException) { }
        catch (TimeoutException) { throw new Exception("A cancelled compile did not stop within 10 seconds."); }
        Console.WriteLine($"A cancelled compile stopped after {clock.Elapsed.TotalMilliseconds:0} ms.");
    }

    private static string FindSources()
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
            if (File.Exists(Path.Combine(directory.FullName, "recomp", "runtime", "rt.h"))) return directory.FullName;
        throw new Exception("Could not find the repository's recomp/runtime from " + AppContext.BaseDirectory);
    }

    private static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
}
