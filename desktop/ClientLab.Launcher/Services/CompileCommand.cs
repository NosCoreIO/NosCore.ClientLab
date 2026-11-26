namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// <c>ClientLab --compile &lt;generated C folder&gt; &lt;output folder&gt; [--emcc &lt;compiler&gt;] [--data-dir ...]</c>:
/// the launcher's own WebAssembly build (object cache, workers, runtime module reuse) for recompiler output produced
/// outside the launcher, such as <c>recomp/lift.py</c> or <c>ClientLab --recompile</c>. The output folder can be served
/// with <c>ClientLab --serve --build</c>.
/// <para><c>ClientLab --build-host &lt;output folder&gt; [--emcc &lt;compiler&gt;]</c> builds only the runtime module, with the
/// <c>host.json</c> stamp; the release workflow embeds it so players compile only the game module.</para>
/// </summary>
internal static class CompileCommand
{
    public static async Task<int> RunAsync(string[] args, string dataDirectory)
    {
        if (args.Length < 3 || args[1].StartsWith("--") || args[2].StartsWith("--"))
        {
            Console.Error.WriteLine("usage: ClientLab --compile <generated C folder> <output folder> [--emcc <compiler>] [--data-dir <folder>]");
            return 2;
        }
        var generated = Path.GetFullPath(args[1]); var output = Path.GetFullPath(args[2]);
        if (!Directory.EnumerateFiles(generated, "*.c").Any())
        {
            Console.Error.WriteLine($"No generated C files in {generated}.");
            return 2;
        }
        return await WithCompilerAsync(dataDirectory, async (app, compiler, token) =>
        {
            await WasmCompiler.BuildAsync(SourceAssets.Resolve(), generated, output, compiler, app, token);
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

    private static async Task<int> WithCompilerAsync(string dataDirectory, Func<LauncherCoordinator, string, CancellationToken, Task> build)
    {
        await using var app = new LauncherCoordinator(SourceAssets.Resolve(), dataDirectory);
        app.Logged += line => Console.WriteLine(line);
        var compiler = Program.Option("--emcc") ?? app.Snapshot()["settings"]?["emcc"]?.GetValue<string>() ?? "";
        if (!File.Exists(compiler))
        {
            Console.Error.WriteLine("No WebAssembly compiler: pass --emcc <path to emcc>, or let the launcher install its tools once.");
            return 2;
        }
        using var stop = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; stop.Cancel(); };
        try
        {
            await build(app, compiler, stop.Token);
            return 0;
        }
        catch (Exception error) when (error is InvalidOperationException or InvalidDataException or OperationCanceledException)
        {
            Console.Error.WriteLine(error.Message);
            return 1;
        }
    }
}
