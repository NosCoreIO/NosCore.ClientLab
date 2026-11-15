namespace NosCore.ClientLab.Launcher.Services;

internal static class WasmCompiler
{
    public static async Task BuildAsync(string root, string generated, string output, string compiler, LauncherCoordinator app, CancellationToken token)
    {
        var runtime = Path.Combine(root, "recomp/runtime"); var port = Path.Combine(root, "recomp/port");
        var sources = Directory.GetFiles(generated, "*.c").Concat([Path.Combine(runtime, "rt.c")]).Concat(Directory.GetFiles(port, "*.c")).Order().ToArray();
        var objects = Path.Combine(Path.GetDirectoryName(output)!, "obj"); Directory.CreateDirectory(objects); Directory.CreateDirectory(output);
        var dependencies = Directory.GetFiles(port).Concat(Directory.GetFiles(runtime)).Where(path => Path.GetExtension(path) is ".h" or ".inc").ToArray();
        var dependencyTime = dependencies.Max(File.GetLastWriteTimeUtc);
        var completed = 0;
        await Parallel.ForEachAsync(sources, new ParallelOptions { MaxDegreeOfParallelism = Math.Clamp(Environment.ProcessorCount - 1, 1, 4), CancellationToken = token }, async (source, cancellation) =>
        {
            var obj = Path.Combine(objects, Path.GetFileNameWithoutExtension(source) + ".o");
            if (!File.Exists(obj) || File.GetLastWriteTimeUtc(obj) < File.GetLastWriteTimeUtc(source) || File.GetLastWriteTimeUtc(obj) < dependencyTime)
                await app.RunAsync(compiler, ["-O2", "-w", "-c", "-sSUPPORT_LONGJMP=0", "-I", runtime, "-I", port, source, "-o", obj], cancellation);
            var done = Interlocked.Increment(ref completed); app.Progress(done / (double)sources.Length);
        });
        app.Status("compiling", "Linking the embedded game runtime…");
        string[] flags = ["-O2", "-sGLOBAL_BASE=536870912", "-sINITIAL_MEMORY=1024MB", "-sALLOW_MEMORY_GROWTH=1", "-sMAXIMUM_MEMORY=2048MB", "-sSTACK_SIZE=64MB", "-sEXIT_RUNTIME=1", "-sSUPPORT_LONGJMP=0", "-sENVIRONMENT=worker", "-sFORCE_FILESYSTEM=1", "-sINVOKE_RUN=0", "-sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV"];
        var arguments = flags.Concat(sources.Select(source => Path.Combine(objects, Path.GetFileNameWithoutExtension(source) + ".o"))).Concat(["-o", Path.Combine(output, "nostale.js")]);
        var responseFile = Path.Combine(Path.GetDirectoryName(output)!, "link.rsp");
        await File.WriteAllLinesAsync(responseFile, arguments.Select(value => "\"" + value.Replace('\\', '/').Replace("\"", "\\\"") + "\""), token);
        await app.RunAsync(compiler, ["@" + responseFile], token);
        foreach (var file in new[] { "nostale.js", "nostale.wasm" })
            if (!File.Exists(Path.Combine(output, file)) || new FileInfo(Path.Combine(output, file)).Length == 0) throw new InvalidDataException("The compiler did not produce a complete runtime.");
    }
}
