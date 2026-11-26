namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// <c>ClientLab --compile &lt;generated C folder&gt; &lt;output folder&gt; [--emcc &lt;compiler&gt;] [--data-dir ...]</c>:
/// the launcher's own WebAssembly build (object cache, workers, runtime module reuse) for recompiler output produced
/// outside the launcher, such as <c>recomp/lift.py</c> or <c>ClientLab --recompile</c>. The output folder can be served
/// with <c>ClientLab --serve --build</c>. <c>--compiler wasm</c> uses the WebAssembly toolchain (no Emscripten), which needs
/// the runtime module of a release build or a <c>Prebuilt/host</c> folder from <c>--build-host</c>.
/// <para><c>ClientLab --build-host &lt;output folder&gt; [--emcc &lt;compiler&gt;]</c> builds only the runtime module, with the
/// <c>host.json</c> stamp; the release workflow embeds it so players compile only the game module.</para>
/// </summary>
internal static class CompileCommand
{
    public static async Task<int> RunAsync(string[] args, string dataDirectory)
    {
        if (args.Length < 3 || args[1].StartsWith("--") || args[2].StartsWith("--"))
        {
            Console.Error.WriteLine("usage: ClientLab --compile <generated C folder> <output folder> [--compiler wasm | --emcc <compiler>] [--data-dir <folder>]");
            return 2;
        }
        var generated = Path.GetFullPath(args[1]); var output = Path.GetFullPath(args[2]);
        if (!Directory.EnumerateFiles(generated, "*.c").Any())
        {
            Console.Error.WriteLine($"No generated C files in {generated}.");
            return 2;
        }
        var choice = LauncherCoordinator.CompilerSetting(Program.Option("--compiler"));
        if (choice == CompilerChoice.WebAssembly)
            return await RunWithAppAsync(dataDirectory, async (app, token) =>
            {
                await WasmCompiler.BuildAsync(SourceAssets.Resolve(), generated, output, choice,
                    _ => throw new InvalidOperationException("Emscripten is not used with --compiler wasm."), app, token);
                Console.WriteLine("Built " + output);
            });
        return await WithCompilerAsync(dataDirectory, async (app, compiler, token) =>
        {
            await WasmCompiler.BuildAsync(SourceAssets.Resolve(), generated, output, CompilerChoice.Emscripten, _ => Task.FromResult(compiler), app, token);
            Console.WriteLine("Built " + output);
        });
    }

    public static async Task<int> RunHostAsync(string[] args, string dataDirectory)
    {
        if (args.Length < 2 || args[1].StartsWith("--"))
        {
            Console.Error.WriteLine("usage: ClientLab --build-host <output folder> [--emcc <compiler>] [--data-dir <folder>]");
            return 2;
        }
        var output = Path.GetFullPath(args[1]);
        return await WithCompilerAsync(dataDirectory, async (app, compiler, token) =>
        {
            await WasmCompiler.BuildHostAsync(SourceAssets.Resolve(), output, compiler, app, token);
            Console.WriteLine("Built the runtime module in " + output);
        });
    }

    private static Task<int> WithCompilerAsync(string dataDirectory, Func<LauncherCoordinator, string, CancellationToken, Task> build) =>
        RunWithAppAsync(dataDirectory, (app, token) =>
        {
            var compiler = Program.Option("--emcc") ?? app.Snapshot()["settings"]?["emcc"]?.GetValue<string>() ?? "";
            if (!File.Exists(compiler))
                throw new InvalidOperationException("No Emscripten: pass --emcc <path to emcc>, let the launcher install its tools once, or use --compiler wasm.");
            return build(app, compiler, token);
        });

    private static async Task<int> RunWithAppAsync(string dataDirectory, Func<LauncherCoordinator, CancellationToken, Task> build)
    {
        await using var app = new LauncherCoordinator(SourceAssets.Resolve(), dataDirectory);
        app.Logged += line => Console.WriteLine(line);
        using var stop = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; stop.Cancel(); };
        try
        {
            await build(app, stop.Token);
            return 0;
        }
        catch (Exception error) when (error is InvalidOperationException or InvalidDataException or OperationCanceledException)
        {
            Console.Error.WriteLine(error.Message);
            return 1;
        }
    }
}
