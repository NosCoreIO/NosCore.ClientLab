using System.Collections.Concurrent;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;

namespace NosCore.ClientLab.Launcher.Services;

internal static class WasmCompiler
{
    public static async Task BuildAsync(string root, string generated, string output, string compiler, LauncherCoordinator app, CancellationToken token)
    {
        var runtime = Path.Combine(root, "recomp/runtime"); var port = Path.Combine(root, "recomp/port");
        var sources = Directory.GetFiles(generated, "*.c").Concat([Path.Combine(runtime, "rt.c")]).Concat(Directory.GetFiles(port, "*.c"))
            .OrderByDescending(path => new FileInfo(path).Length).ThenBy(path => path, StringComparer.Ordinal).ToArray();
        Directory.CreateDirectory(output);
        static string[] Headers(string directory) => Directory.GetFiles(directory).Where(path => Path.GetExtension(path) is ".h" or ".inc").ToArray();
        string[] runtimeHeaders = Headers(runtime), portHeaders = Headers(port);
        // The game is a side module (nostale-game.wasm) and the runtime + portable host a main module (nostale.js/.wasm)
        // that finds the game's label table by name (RT_HOST_MODULE), so both halves are position-independent. The host
        // module does not depend on the client at all: it relinks only when its own code changes.
        string[] compileFlags = ["-O2", "-w", "-c", "-fPIC", "-sSUPPORT_LONGJMP=0"];
        // SIMD applies to portable host math/state copies.
        string[] hostCompileFlags = ["-O3", "-msimd128", "-w", "-c", "-fPIC", "-DRT_HOST_MODULE", "-sSUPPORT_LONGJMP=0"];
        async Task<string> IdentityAsync(IEnumerable<string> dependencies, string[] compilerOptions)
        {
            using var fingerprint = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            fingerprint.AppendData(Encoding.UTF8.GetBytes("wasm-objects-v1\0" + string.Join(' ', compilerOptions) + "\0" + Path.GetFullPath(compiler) + "\0" +
                System.Runtime.InteropServices.RuntimeInformation.RuntimeIdentifier + "\0" + Environment.GetEnvironmentVariable("EMCC_CFLAGS")));
            foreach (var dependency in dependencies.Order(StringComparer.Ordinal).Prepend(compiler))
            {
                fingerprint.AppendData(Encoding.UTF8.GetBytes(Path.GetFileName(dependency) + "\0"));
                await using var input = File.OpenRead(dependency);
                fingerprint.AppendData(await SHA256.HashDataAsync(input, token));
            }
            return Convert.ToHexStringLower(fingerprint.GetHashAndReset());
        }
        // Generated code includes only rt.h, so its objects (nearly all of the compile time) survive changes to the
        // portable host's headers, such as the renderer; host objects depend on both sets of headers.
        var cacheRoot = Path.Combine(app.DataDirectory, "cache", "wasm-objects");
        var gameCache = new CompilationCache(cacheRoot, await IdentityAsync(runtimeHeaders, compileFlags));
        var hostCache = new CompilationCache(cacheRoot, await IdentityAsync(portHeaders.Concat(runtimeHeaders), hostCompileFlags));
        var generatedRoot = Path.GetFullPath(generated) + Path.DirectorySeparatorChar;
        var memory = GC.GetGCMemoryInfo();
        var budget = Math.Max(0, memory.TotalAvailableMemoryBytes - memory.MemoryLoadBytes);
        var workers = Math.Clamp(Math.Min(Environment.ProcessorCount - 2, (int)(budget / (3L * 1024 * 1024 * 1024))), 1, 8);
        var objects = new ConcurrentDictionary<string, string>();
        var completed = 0; var reused = 0;
        var elapsed = Stopwatch.StartNew();
        using var heartbeat = new CancellationTokenSource();
        var progress = Task.Run(async () =>
        {
            using var timer = new PeriodicTimer(TimeSpan.FromSeconds(1));
            try
            {
                while (await timer.WaitForNextTickAsync(heartbeat.Token))
                    app.Status("compiling", $"Preparing game: {Volatile.Read(ref completed)}/{sources.Length} files · {(int)elapsed.Elapsed.TotalMinutes}:{elapsed.Elapsed.Seconds:00}", .9 * Volatile.Read(ref completed) / sources.Length);
            }
            catch (OperationCanceledException) { }
        });
        app.Log($"Compiling {sources.Length} files with {workers} workers; completed objects are cached across build attempts.");
        try
        {
            await Parallel.ForEachAsync(sources, new ParallelOptions { MaxDegreeOfParallelism = workers, CancellationToken = token }, async (source, cancellation) =>
            {
                var watch = Stopwatch.StartNew();
                var gameSource = Path.GetFullPath(source).StartsWith(generatedRoot, StringComparison.OrdinalIgnoreCase);
                var cache = gameSource ? gameCache : hostCache;
                var result = await cache.GetOrCompileAsync(source, (obj, ct) => app.RunAsync(compiler,
                    (gameSource ? compileFlags : hostCompileFlags).Concat(["-I", runtime, "-I", port, source, "-o", obj]), ct), cancellation);
                objects[source] = result.Path;
                if (result.Reused) Interlocked.Increment(ref reused);
                app.Log($"{(result.Reused ? "Reused" : "Compiled")} {Path.GetFileName(source)} ({watch.Elapsed.TotalSeconds:F1}s)");
                Interlocked.Increment(ref completed);
            });
        }
        finally { heartbeat.Cancel(); await progress; }
        app.Log($"Reused {reused} cached files; compiled {sources.Length - reused} files in {elapsed.Elapsed.TotalSeconds:F0}s.");
        app.Status("compiling", "Finishing game preparation…", .9);
        var ordered = sources.Order(StringComparer.Ordinal).ToArray();
        var gameObjects = ordered.Where(source => Path.GetFullPath(source).StartsWith(generatedRoot, StringComparison.OrdinalIgnoreCase)).Select(source => objects[source]).ToArray();
        var hostObjects = ordered.Except(ordered.Where(source => Path.GetFullPath(source).StartsWith(generatedRoot, StringComparison.OrdinalIgnoreCase))).Select(source => objects[source]).ToArray();
        await Task.WhenAll(
            LinkAsync(compiler, ["-O2", "-sSIDE_MODULE=1", "-sSUPPORT_LONGJMP=0"], gameObjects, Path.Combine(output, "nostale-game.wasm"), app, token),
            HostModuleAsync(compiler, hostObjects, Path.Combine(app.DataDirectory, "cache", "host-module"), output, app, token));
        foreach (var file in new[] { "nostale.js", "nostale.wasm", "nostale-game.wasm" })
            if (!File.Exists(Path.Combine(output, file)) || new FileInfo(Path.Combine(output, file)).Length == 0) throw new InvalidDataException("The compiler did not produce a complete runtime.");
    }

    private static readonly string[] HostLinkFlags = ["-O3", "-msimd128", "-sMAIN_MODULE=1", "-sGLOBAL_BASE=536870912", "-sINITIAL_MEMORY=1024MB", "-sALLOW_MEMORY_GROWTH=1",
        "-sMAXIMUM_MEMORY=2048MB", "-sSTACK_SIZE=64MB", "-sEXIT_RUNTIME=1", "-sSUPPORT_LONGJMP=0", "-sENVIRONMENT=worker", "-sFORCE_FILESYSTEM=1", "-sINVOKE_RUN=0",
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV", "--emit-symbol-map"];

    /// <summary>The runtime main module, linked once per distinct host build: its objects are content-addressed, so their
    /// names identify it. A client patch reuses it as is.</summary>
    private static async Task HostModuleAsync(string compiler, string[] hostObjects, string cache, string output, LauncherCoordinator app, CancellationToken token)
    {
        var key = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join('\n', HostLinkFlags.Concat(hostObjects.Select(ObjectIdentity)).Prepend(Path.GetFullPath(compiler))))))[..24];
        var cached = Path.Combine(cache, key);
        string[] files = ["nostale.js", "nostale.wasm", "nostale.js.symbols"];
        if (!File.Exists(Path.Combine(cached, "nostale.wasm")))
        {
            var staging = cached + "." + Guid.NewGuid().ToString("N");
            Directory.CreateDirectory(staging);
            try
            {
                await LinkAsync(compiler, HostLinkFlags, hostObjects, Path.Combine(staging, "nostale.js"), app, token);
                try { Directory.Move(staging, cached); }
                catch (IOException) when (File.Exists(Path.Combine(cached, "nostale.wasm"))) { } // Another build linked it first.
            }
            finally { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); }
        }
        else app.Log("Reused the runtime module; only the game module was linked.");
        foreach (var file in files.Where(file => File.Exists(Path.Combine(cached, file))))
            File.Copy(Path.Combine(cached, file), Path.Combine(output, file), overwrite: true);
    }

    /// <summary>An object's cache directory (its headers, flags and compiler) and file name (its source).</summary>
    private static string ObjectIdentity(string obj) => Path.GetFileName(Path.GetDirectoryName(obj)) + "/" + Path.GetFileName(obj);

    private static async Task LinkAsync(string compiler, IEnumerable<string> flags, IEnumerable<string> objects, string target, LauncherCoordinator app, CancellationToken token)
    {
        var responseFile = target + ".rsp";
        await File.WriteAllLinesAsync(responseFile, flags.Concat(objects).Concat(["-o", target]).Select(value => "\"" + value.Replace('\\', '/').Replace("\"", "\\\"") + "\""), token);
        try { await app.RunAsync(compiler, ["@" + responseFile], token); }
        finally { File.Delete(responseFile); }
    }
}
