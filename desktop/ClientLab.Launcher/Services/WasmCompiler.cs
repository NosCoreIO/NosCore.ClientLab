using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace NosCore.ClientLab.Launcher.Services;

public static class WasmCompiler
{
    // The game is a side module (nostale-game.wasm) and the runtime + portable host a main module (nostale.js/.wasm)
    // that finds the game's label table by name (RT_HOST_MODULE), so both halves are position-independent. The host
    // module does not depend on the client at all: releases ship it prebuilt (HostBundle), and a local build relinks it
    // only when its own code changes.
    private static readonly string[] GameCompileFlags = ["-O2", "-w", "-c", "-fPIC", "-sSUPPORT_LONGJMP=0"];
    // SIMD applies to portable host math/state copies.
    private static readonly string[] HostCompileFlags = ["-O3", "-msimd128", "-w", "-c", "-fPIC", "-DRT_HOST_MODULE", "-sSUPPORT_LONGJMP=0"];
    private static readonly string[] GameLinkFlags = ["-O2", "-sSIDE_MODULE=1", "-sSUPPORT_LONGJMP=0"];
    private static readonly string[] HostLinkFlags = ["-O3", "-msimd128", "-sMAIN_MODULE=1", "-sGLOBAL_BASE=536870912", "-sINITIAL_MEMORY=1024MB", "-sALLOW_MEMORY_GROWTH=1",
        "-sMAXIMUM_MEMORY=2048MB", "-sSTACK_SIZE=64MB", "-sEXIT_RUNTIME=1", "-sSUPPORT_LONGJMP=0", "-sENVIRONMENT=worker", "-sFORCE_FILESYSTEM=1", "-sINVOKE_RUN=0",
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV", "--emit-symbol-map"];
    public static readonly string[] HostFiles = ["nostale.js", "nostale.wasm", "nostale.js.symbols"];

    /// <summary>Builds the game module for <paramref name="generated"/> and places the runtime module beside it: the
    /// release's prebuilt one when it was built from these exact sources with this Emscripten version, otherwise a local one.</summary>
    public static async Task BuildAsync(string root, string generated, string output, string compiler, LauncherCoordinator app, CancellationToken token, HostBundle? bundle = null)
    {
        Directory.CreateDirectory(output);
        var prebuilt = await (bundle ?? HostBundle.Embedded).TryInstallAsync(root, compiler, output, app, token);
        var gameSources = Directory.GetFiles(generated, "*.c");
        var hostSources = prebuilt ? [] : HostSources(root);
        var objects = await CompileAsync(root, gameSources, hostSources, compiler, app, token);
        app.Status("compiling", "Finishing game preparation…", .9);
        await Task.WhenAll(
            LinkAsync(compiler, GameLinkFlags, gameSources.Order(StringComparer.Ordinal).Select(source => objects[source]), Path.Combine(output, "nostale-game.wasm"), app, token),
            prebuilt ? Task.CompletedTask : HostModuleAsync(compiler, hostSources.Order(StringComparer.Ordinal).Select(source => objects[source]).ToArray(), output, app, token));
        foreach (var file in new[] { "nostale.js", "nostale.wasm", "nostale-game.wasm" })
            if (!File.Exists(Path.Combine(output, file)) || new FileInfo(Path.Combine(output, file)).Length == 0) throw new InvalidDataException("The compiler did not produce a complete runtime.");
    }

    /// <summary>Builds only the runtime module, plus the <c>host.json</c> stamp that lets a launcher with the same sources
    /// use it instead of compiling its own (<c>ClientLab --build-host</c>, run by the release workflow).</summary>
    public static async Task BuildHostAsync(string root, string output, string compiler, LauncherCoordinator app, CancellationToken token)
    {
        var version = await EmscriptenVersionAsync(compiler, token)
            ?? throw new InvalidOperationException($"Could not read the Emscripten version of {compiler}.");
        if (version != ToolchainInstaller.EmsdkVersion)
            throw new InvalidOperationException($"The runtime module must be built with Emscripten {ToolchainInstaller.EmsdkVersion}, the version launchers install; {compiler} is {version}.");
        Directory.CreateDirectory(output);
        var hostSources = HostSources(root);
        var objects = await CompileAsync(root, [], hostSources, compiler, app, token);
        await HostModuleAsync(compiler, hostSources.Order(StringComparer.Ordinal).Select(source => objects[source]).ToArray(), output, app, token);
        var stamp = new JsonObject { ["fingerprint"] = HostFingerprint(root, version), ["emscripten"] = version };
        foreach (var file in HostFiles)
            stamp[file] = Convert.ToHexStringLower(SHA256.HashData(await File.ReadAllBytesAsync(Path.Combine(output, file), token)));
        await File.WriteAllTextAsync(Path.Combine(output, HostBundle.StampFile), stamp.ToJsonString(new() { WriteIndented = true }), token);
    }

    private static string[] HostSources(string root) =>
        [Path.Combine(root, "recomp/runtime/rt.c"), .. Directory.GetFiles(Path.Combine(root, "recomp/port"), "*.c")];

    /// <summary>Identifies a runtime module by everything it is built from: the runtime and portable host sources (line
    /// endings normalised, so a Windows checkout matches a Linux build), the flags and the Emscripten version.</summary>
    public static string HostFingerprint(string root, string emscriptenVersion)
    {
        using var fingerprint = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        fingerprint.AppendData(Encoding.UTF8.GetBytes("host-module-v1\0" + emscriptenVersion + "\0" + string.Join(' ', HostCompileFlags) + "\0" + string.Join(' ', HostLinkFlags) + "\0"));
        foreach (var directory in new[] { "recomp/runtime", "recomp/port" })
            foreach (var file in Directory.GetFiles(Path.Combine(root, directory)).Where(path => Path.GetExtension(path) is ".c" or ".h" or ".inc")
                         .Order(StringComparer.Ordinal))
            {
                fingerprint.AppendData(Encoding.UTF8.GetBytes(directory + "/" + Path.GetFileName(file) + "\0"));
                fingerprint.AppendData(File.ReadAllBytes(file).Where(value => value != (byte)'\r').ToArray());
                fingerprint.AppendData([0]);
            }
        return Convert.ToHexStringLower(fingerprint.GetHashAndReset());
    }

    /// <summary>The version emcc reports, or null when it cannot be run.</summary>
    public static async Task<string?> EmscriptenVersionAsync(string compiler, CancellationToken token)
    {
        try
        {
            var info = new ProcessStartInfo(compiler, "--version") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            using var child = Process.Start(info);
            if (child is null) return null;
            var stdout = child.StandardOutput.ReadToEndAsync(token); var stderr = child.StandardError.ReadToEndAsync(token);
            await child.WaitForExitAsync(token);
            var match = Regex.Match(await stdout + "\n" + await stderr, @"emcc \(Emscripten[^)]*\) (\d+\.\d+\.\d+)");
            return child.ExitCode == 0 && match.Success ? match.Groups[1].Value : null;
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException) { return null; }
    }

    private static async Task<ConcurrentDictionary<string, string>> CompileAsync(string root, string[] gameSources, string[] hostSources, string compiler, LauncherCoordinator app, CancellationToken token)
    {
        var runtime = Path.Combine(root, "recomp/runtime"); var port = Path.Combine(root, "recomp/port");
        var game = gameSources.ToHashSet(StringComparer.Ordinal);
        var sources = gameSources.Concat(hostSources).OrderByDescending(path => new FileInfo(path).Length).ThenBy(path => path, StringComparer.Ordinal).ToArray();
        static string[] Headers(string directory) => Directory.GetFiles(directory).Where(path => Path.GetExtension(path) is ".h" or ".inc").ToArray();
        string[] runtimeHeaders = Headers(runtime), portHeaders = Headers(port);
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
        var gameCache = new CompilationCache(cacheRoot, await IdentityAsync(runtimeHeaders, GameCompileFlags));
        var hostCache = new CompilationCache(cacheRoot, await IdentityAsync(portHeaders.Concat(runtimeHeaders), HostCompileFlags));
        var memory = GC.GetGCMemoryInfo();
        var budget = Math.Max(0, memory.TotalAvailableMemoryBytes - memory.MemoryLoadBytes);
        var workers = Math.Clamp(Math.Min(Environment.ProcessorCount - 2, (int)(budget / (3L * 1024 * 1024 * 1024))), 1, 8);
        var objects = new ConcurrentDictionary<string, string>(StringComparer.Ordinal);
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
                var gameSource = game.Contains(source);
                var result = await (gameSource ? gameCache : hostCache).GetOrCompileAsync(source, (obj, ct) => app.RunAsync(compiler,
                    (gameSource ? GameCompileFlags : HostCompileFlags).Concat(["-I", runtime, "-I", port, source, "-o", obj]), ct), cancellation);
                objects[source] = result.Path;
                if (result.Reused) Interlocked.Increment(ref reused);
                app.Log($"{(result.Reused ? "Reused" : "Compiled")} {Path.GetFileName(source)} ({watch.Elapsed.TotalSeconds:F1}s)");
                Interlocked.Increment(ref completed);
            });
        }
        finally { heartbeat.Cancel(); await progress; }
        app.Log($"Reused {reused} cached files; compiled {sources.Length - reused} files in {elapsed.Elapsed.TotalSeconds:F0}s.");
        return objects;
    }

    /// <summary>The runtime main module, linked once per distinct host build: its objects are content-addressed, so their
    /// names identify it. A client patch reuses it as is.</summary>
    private static async Task HostModuleAsync(string compiler, string[] hostObjects, string output, LauncherCoordinator app, CancellationToken token)
    {
        var key = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join('\n', HostLinkFlags.Concat(hostObjects.Select(ObjectIdentity)).Prepend(Path.GetFullPath(compiler))))))[..24];
        var cached = Path.Combine(app.DataDirectory, "cache", "host-module", key);
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
        foreach (var file in HostFiles.Where(file => File.Exists(Path.Combine(cached, file))))
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

/// <summary>A runtime module built ahead of time (by the release workflow) with its <c>host.json</c> stamp.</summary>
public sealed class HostBundle(Func<string, Stream?> open, string description)
{
    public const string StampFile = "host.json";
    private const string ResourcePrefix = "Prebuilt/host/";

    /// <summary>The module embedded in this launcher build; empty in local developer builds.</summary>
    public static HostBundle Embedded { get; } = new(name => Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourcePrefix + name), "the launcher's bundled runtime module");

    public static HostBundle FromDirectory(string directory) =>
        new(name => File.Exists(Path.Combine(directory, name)) ? File.OpenRead(Path.Combine(directory, name)) : null, directory);

    /// <summary>Copies the module into <paramref name="output"/> when it matches these sources and compiler, and says why not otherwise.</summary>
    public async Task<bool> TryInstallAsync(string root, string compiler, string output, LauncherCoordinator app, CancellationToken token)
    {
        JsonNode? stamp;
        await using (var stream = open(StampFile))
        {
            if (stream is null) return false;
            stamp = await JsonNode.ParseAsync(stream, cancellationToken: token);
        }
        if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable("EMCC_CFLAGS")))
        {
            app.Log("EMCC_CFLAGS is set, so the runtime module is built locally instead of using " + description + ".");
            return false;
        }
        var expected = stamp?["emscripten"]?.GetValue<string>();
        var version = await WasmCompiler.EmscriptenVersionAsync(compiler, token);
        if (version is null || version != expected)
        {
            app.Log($"The compiler is Emscripten {version ?? "(unknown)"}, but {description} needs {expected}; building the runtime module locally.");
            return false;
        }
        if (stamp?["fingerprint"]?.GetValue<string>() != WasmCompiler.HostFingerprint(root, version))
        {
            app.Log($"The runtime sources differ from {description}; building the runtime module locally.");
            return false;
        }
        foreach (var file in WasmCompiler.HostFiles)
        {
            await using var input = open(file) ?? throw new InvalidDataException($"{description} is missing {file}.");
            var target = Path.Combine(output, file);
            string actual;
            await using (var copy = File.Create(target + ".part"))
            using (var hasher = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
            {
                var buffer = new byte[1 << 20];
                int length;
                while ((length = await input.ReadAsync(buffer, token)) > 0)
                {
                    hasher.AppendData(buffer, 0, length);
                    await copy.WriteAsync(buffer.AsMemory(0, length), token);
                }
                actual = Convert.ToHexStringLower(hasher.GetHashAndReset());
            }
            if (actual != stamp![file]?.GetValue<string>())
            {
                File.Delete(target + ".part");
                throw new InvalidDataException($"{file} in {description} does not match its checksum.");
            }
            File.Move(target + ".part", target, overwrite: true);
        }
        app.Log($"Using {description} (Emscripten {version}); only the game module is compiled here.");
        return true;
    }
}
